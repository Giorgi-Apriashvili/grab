#include "folder_job.hpp"

#include "cli.hpp"
#include "existing.hpp"
#include "local_name.hpp"
#include "process.hpp"
#include "rclone.hpp"
#include "rclone_encoding.hpp"
#include "rclone_flags.hpp"
#include "server_ops.hpp"
#include "util.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)
#else
#include <unistd.h>
#endif

namespace grab::folder {

namespace fs = std::filesystem;

namespace {

void notify(const Job& job, Event e) {
    if (job.on_event) job.on_event(e);
}

unsigned long process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(getpid());
#endif
}

// The queued small files (`batch_files`: their indexes) in one rclone copy: its connection pool
// is reused for every file, where a new SSH login per small file would dominate. Pausing or
// skipping one of them restarts the batch without it.
void run_batch(const Job& job, Tree& tree, std::mutex& m, const std::vector<std::size_t>& batch_files,
               const std::stop_token& stop) {
    const int reserved = job.connections->reserve(job.server, job.id, stop);
    if (reserved == 0) return; // stopped while waiting
    const auto list = fs::temp_directory_path() / std::format("grab-files-{}-{}.txt", process_id(), job.id);
    std::vector<std::string> paths;
    {
        std::lock_guard lock(m);
        for (const auto i : batch_files) paths.push_back(tree.files[i].path);
    }
    std::unordered_map<std::string, std::size_t> index; // path -> file, for the names rclone reports
    if (auto written = util::write_file_atomic(list, files_from_text(paths)); !written) {
        job.connections->release(job.server, job.id, reserved);
        std::lock_guard lock(m);
        for (const auto i : batch_files) {
            auto& f = tree.files[i];
            if (f.status != State::queued) continue;
            f.status = State::failed;
            f.error = written.error();
        }
        return;
    }
    const std::uint64_t limit = job.batch_limit ? job.batch_limit() : 0;

    std::stop_source batch;
    {
        std::lock_guard lock(m);
        for (std::size_t i = 0; i < tree.files.size(); ++i) index.emplace(tree.files[i].path, i);
        for (const auto i : batch_files) {
            auto& f = tree.files[i];
            if (f.status != State::queued) continue;
            f.in_batch = true; // stays queued until rclone starts it
            f.bytes = 0;
            f.error.clear();
        }
        tree.batch_stop = batch;
        tree.batch_running = true;
        tree.connections = job.connections->active(job.server, job.id);
    }
    notify(job, Event::changed);
    std::stop_callback forward(stop, [&batch] { batch.request_stop(); });
    proc::RunOptions run;
    run.detached = true;
    run.stop = batch.get_token();
    std::vector<std::string> extra{"--files-from0", util::path_to_utf8(list), "--transfers",
                                   std::to_string(reserved), "--sftp-connections", std::to_string(reserved), "-v"};
    // rclone writes these files itself, so the speed limit reaches it as --bwlimit; its bytes
    // are also charged to the shared limiter, which slows grab's own streams to match.
    if (limit > 0) extra.insert(extra.end(), {"--bwlimit", rate::bwlimit_arg(limit)});
    // Conflicts were answered before: a file that appeared at one of these names since stays.
    if (job.existing != existing::Choice::replace) extra.emplace_back("--ignore-existing");
    const auto given = rclone_flags::connection(job.extra);
    extra.insert(extra.end(), given.begin(), given.end());
    // What rclone leaves when stopped: its temp files next to these, made by this run.
    std::vector<fs::path> targets;
    targets.reserve(paths.size());
    const auto dir = job_dir(job);
    {
        std::lock_guard lock(m);
        for (const auto& f : tree.files) {
            if (f.status == State::queued && !f.big) targets.push_back(target_of(dir, f));
        }
    }
    const auto temp_before = engine::rclone_temp_files(targets);
    std::uint64_t charged = 0;
    bool worked = false; // its connections count as working (fetch::Connections::working)
    auto last_check = std::chrono::steady_clock::now();
    auto result = engine::download(
        *job.ctx, Mode::folder, job.remote_path, job.dest,
        [&](const engine::Progress& p) {
            if (p.bytes > charged) {
                if (job.limiter != nullptr) job.limiter->debit(p.bytes - charged);
                charged = p.bytes;
            }
            const int conns = job.connections->active(job.server, job.id);
            {
                std::lock_guard lock(m);
                for (const auto& t : p.transferring) {
                    auto it = index.find(t.name);
                    if (it == index.end()) continue;
                    auto& f = tree.files[it->second];
                    if (!f.in_batch || (f.status != State::queued && f.status != State::running)) continue;
                    f.status = State::running;
                    f.bytes = t.bytes;
                    f.speed = t.speed;
                    f.eta = t.speed > 0 && t.size > t.bytes ? std::optional(static_cast<double>(t.size - t.bytes) / t.speed)
                                                            : std::nullopt;
                }
                tree.connections = conns;
            }
            if (p.bytes > 0 && !worked) {
                worked = true;
                job.connections->working(job.server, job.id, reserved);
            }
            // Its connections follow its share (review G192): when another download waits for its
            // own while this holds more, or two or more are free within its share, rclone starts
            // again with the new number; only the files in flight start over.
            if (const auto now = std::chrono::steady_clock::now(); now - last_check >= std::chrono::seconds(3)) {
                last_check = now;
                if (job.connections->unbalanced(job.server, job.id, reserved)) batch.request_stop();
            }
            notify(job, Event::progress);
        },
        run, extra, job.on_command,
        [&](const std::string& name) {
            {
                std::lock_guard lock(m);
                auto it = index.find(name);
                if (it == index.end()) return;
                auto& f = tree.files[it->second];
                if (!f.in_batch || (f.status != State::queued && f.status != State::running)) return;
                f.status = State::done;
                f.fetched = true;
                f.bytes = f.size;
                f.speed = 0;
                f.eta.reset();
            }
            notify(job, Event::progress); // can be many a second in a folder of small files
        },
        job.local_name.empty() ? std::nullopt : std::optional(job.local_name));
    std::error_code ec;
    fs::remove(list, ec);
    // A stopped or failed rclone leaves its temp files (after a clean finish it renamed them all),
    // but never delete one of the folder's own files: they can be named like them.
    if (!result || result->exit_code != 0) {
        fetch::Reserved own_files;
        {
            std::lock_guard lock(m);
            own_files = reserved_names(dir, tree);
        }
        engine::remove_rclone_temp_files(targets, temp_before, own_files);
    }
    job.connections->release(job.server, job.id, reserved, worked);

    std::lock_guard lock(m);
    tree.batch_running = false;
    const bool finished = result && result->exit_code == 0;
    for (const auto i : batch_files) {
        auto& f = tree.files[i];
        const bool was_in_batch = f.in_batch;
        f.in_batch = false;
        if (!was_in_batch || (f.status != State::running && f.status != State::queued)) continue; // done, paused, skipped
        f.speed = 0;
        f.eta.reset();
        if (finished) {
            const auto local = target_of(dir, f);
            if (fs::file_size(local, ec) == f.size && !ec) {
                // rclone didn't say it copied this one: it was there already (kept by
                // --ignore-existing), not written by grab, so verify never deletes it (review G82).
                f.status = State::done;
                f.bytes = f.size;
            } else {
                f.status = State::failed;
                f.error = name_not_utf8(f.path) ? std::string(not_utf8_error) : "missing after the copy";
            }
        } else if (batch.stop_requested()) {
            f.status = State::queued; // restarted without a paused/skipped file, or the folder stopped
            f.bytes = 0;              // and rclone starts it over
        } else {
            f.status = State::failed;
            f.error = !result ? result.error()
                      : result->last_error.empty() ? std::format("rclone exited with code {}", result->exit_code)
                                                   : result->last_error;
        }
    }
}

// The queued big files, up to three at a time, each resumable like a single-file download.
// They share the folder's connections (same pool id) and split ranges to keep them busy.
void run_big(const Job& job, Tree& tree, std::mutex& m, const std::stop_token& stop) {
    constexpr int at_once = 3;
    const auto dir = job_dir(job);
    fetch::Reserved reserved;
    {
        std::lock_guard lock(m);
        reserved = reserved_names(dir, tree);
    }
    auto flags = rclone_flags::connection(job.ctx->settings.common_flags);
    const auto extra = rclone_flags::connection(job.extra);
    flags.insert(flags.end(), extra.begin(), extra.end());
    auto worker = [&] {
        for (;;) {
            if (stop.stop_requested()) return;
            std::size_t i = 0;
            std::stop_source file_stop;
            std::string path;
            fs::path target;
            {
                std::lock_guard lock(m);
                auto it = std::ranges::find_if(tree.files, [](const File& f) { return f.big && f.status == State::queued; });
                if (it == tree.files.end()) return;
                i = static_cast<std::size_t>(it - tree.files.begin());
                it->status = State::running;
                it->error.clear();
                it->stop = file_stop;
                it->stop_reason = StopReason::none;
                path = it->path;
                target = target_of(dir, *it);
            }
            notify(job, Event::changed);
            std::stop_callback forward(stop, [&file_stop] { file_stop.request_stop(); });
            fetch::Download d;
            d.source = fetch::Source{job.ctx->config.rclone, job.ctx->config.rclone_config,
                                     job.ctx->settings.rclone_remote, child_remote_path(job.remote_path, path), flags};
            d.server = job.server;
            d.id = job.id;
            d.target = target;
            d.reserved = reserved;
            d.replace = job.existing == existing::Choice::replace;
            d.joined = true;
            d.limiter = job.limiter;
            d.weight = job.weight;
            d.on_command = job.on_command;
            std::error_code ec;
            fs::create_directories(d.target.parent_path(), ec);
            const auto result = fetch::run(d, *job.connections, [&](const fetch::Progress& p) {
                {
                    std::lock_guard lock(m);
                    auto& f = tree.files[i];
                    f.bytes = p.bytes;
                    f.speed = p.speed;
                    f.eta = p.eta;
                    tree.connections = p.connections;
                }
                notify(job, Event::progress);
            }, file_stop.get_token());

            bool discard = false;
            {
                std::lock_guard lock(m);
                auto& f = tree.files[i];
                f.speed = 0;
                f.eta.reset();
                switch (result.outcome) {
                case fetch::Outcome::done:
                    f.status = State::done;
                    f.fetched = f.fetched || !result.present;
                    f.bytes = f.size;
                    break;
                case fetch::Outcome::failed:
                case fetch::Outcome::exists: // appeared since the question: Retry asks
                    f.status = State::failed;
                    f.error = result.error;
                    break;
                case fetch::Outcome::stopped:
                    f.status = f.stop_reason == StopReason::pause  ? State::paused
                               : f.stop_reason == StopReason::skip ? State::skipped
                                                                   : State::queued; // the folder stopped
                    discard = f.status == State::skipped;
                    break;
                }
            }
            if (discard) fetch::discard(d.target, d.source.path, d.reserved, d.source.rclone_remote);
            notify(job, Event::changed);
        }
    };
    std::vector<std::jthread> workers;
    for (int k = 0; k < at_once; ++k) workers.emplace_back(worker);
}

} // namespace

File make_file(std::string path, std::uint64_t size, std::string modtime) {
    File f;
    f.path = std::move(path);
    f.size = size;
    f.modtime = std::move(modtime);
    // A name grab gives otherwise than rclone anywhere in its path (a device name, a short name,
    // bidi controls): grab fetches it itself, since rclone would write it under rclone's name.
    f.big = is_big(size) || local::differs_from_rclone(f.path);
    return f;
}

void settle_names(std::vector<File>& files) {
    // rclone's batch would copy one of each clashing group: grab fetches them all.
    std::vector<std::string> paths;
    paths.reserve(files.size());
    for (const auto& f : files) paths.push_back(f.path);
    const auto clash = local::clashing(paths);
    // Every file's local path, and every folder on the way to one, as Windows compares them.
    std::vector<fs::path> locals;
    locals.reserve(files.size());
    std::unordered_set<fs::path::string_type> names; // key() of every file's own name
    std::unordered_set<fs::path::string_type> taken; // key() of every folder, then of names given out
    for (const auto& f : files) {
        locals.push_back(local::relative(f.path));
        const auto& rel = locals.back();
        for (auto folder = rel.parent_path(); !folder.empty(); folder = folder.parent_path()) taken.insert(local::key(folder));
        names.insert(local::key(rel));
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
        auto& f = files[i];
        if (clash.contains(f.path)) f.big = true;
        if (taken.insert(local::key(locals[i])).second) continue; // the first with this name
        // An earlier file, or a folder, has this name on Windows: the next free number.
        const std::string name = util::path_to_utf8(locals[i].filename());
        bool given = false;
        for (int n = 2; n < 1000 && !given; ++n) {
            auto numbered = locals[i].parent_path() / util::path_from_utf8(existing::numbered(name, n));
            const auto k = local::key(numbered);
            if (names.contains(k) || !taken.insert(k).second) continue;
            f.local = std::move(numbered);
            f.big = true; // grab writes it under that name
            given = true;
        }
        if (!given) {
            f.status = State::failed;
            f.error = "no free name for it: other files of the folder have its name on Windows";
        }
    }
}

Totals totals(const Tree& tree) {
    Totals t;
    for (const auto& f : tree.files) {
        if (f.status == State::skipped) continue;
        ++t.files;
        t.total += f.size;
        if (f.status == State::done) {
            ++t.files_done;
            t.bytes += f.size;
        } else {
            t.bytes += std::min(f.bytes, f.size);
        }
        if (f.status == State::running) t.speed += f.speed;
    }
    if (t.speed > 0 && t.total > t.bytes) t.eta = static_cast<double>(t.total - t.bytes) / t.speed;
    return t;
}

void pause_file(Tree& tree, const std::string& path) {
    for (auto& f : tree.files) {
        if (f.path != path) continue;
        if (f.status == State::queued) {
            f.status = State::paused;
            if (f.in_batch) tree.batch_stop.request_stop(); // rclone would still fetch it
        } else if (f.status == State::running && f.big) {
            f.stop_reason = StopReason::pause; // run_big marks it paused once stopped
            f.stop.request_stop();
        } else if (f.status == State::running) {
            f.status = State::paused; // in the small batch: restart it without this file
            f.speed = 0;
            f.bytes = 0; // rclone keeps no partial small file; it starts over on resume
            tree.batch_stop.request_stop();
        }
    }
}

void resume_file(Tree& tree, const std::string& path) {
    for (auto& f : tree.files) {
        if (f.path == path && f.status == State::paused) f.status = State::queued;
    }
}

void resume_paused(Tree& tree) {
    for (auto& f : tree.files) {
        if (f.status == State::paused) f.status = State::queued;
    }
}

fetch::Reserved reserved_names(const fs::path& dir, const Tree& tree) {
    auto names = std::make_shared<std::unordered_set<fs::path::string_type>>();
    names->reserve(tree.files.size());
    for (const auto& f : tree.files) {
        names->insert(local::key(dir / local::relative(f.path)));
        if (!f.local.empty()) names->insert(local::key(dir / f.local));
    }
    return [names](const fs::path& p) { return names->contains(local::key(p)); };
}

std::optional<Discard> skip_file(Tree& tree, const fs::path& dir, const std::string& remote_path, const std::string& path) {
    std::optional<Discard> discard;
    for (auto& f : tree.files) {
        if (f.path != path) continue;
        if (f.status == State::queued || f.status == State::paused || f.status == State::failed) {
            if (f.status == State::queued && f.in_batch) tree.batch_stop.request_stop(); // rclone would still fetch it
            f.status = State::skipped;
            if (f.big) discard = Discard{target_of(dir, f), child_remote_path(remote_path, f.path), reserved_names(dir, tree)};
        } else if (f.status == State::running && f.big) {
            f.stop_reason = StopReason::skip; // run_big discards its partial file
            f.stop.request_stop();
        } else if (f.status == State::running) {
            f.status = State::skipped; // restart the batch without it
            f.speed = 0;
            tree.batch_stop.request_stop();
        }
    }
    return discard;
}

void retry_files(Tree& tree, bool all_unfinished) {
    for (auto& f : tree.files) {
        if (f.status == State::failed || (all_unfinished && f.status != State::done && f.status != State::skipped)) {
            f.status = State::queued;
            f.error.clear();
        }
    }
}

Outcome outcome(const Tree& tree) {
    Outcome o;
    for (const auto& f : tree.files) {
        if (f.status == State::failed) {
            if (o.failed++ == 0) o.first_error = f.path + ": " + f.error;
        } else if (f.status == State::paused || f.status == State::queued) {
            ++o.waiting;
        }
    }
    return o;
}

fs::path target_of(const fs::path& dir, const File& f) {
    return dir / (f.local.empty() ? local::relative(f.path) : f.local);
}

fs::path job_dir(const Job& job) {
    return job.local_name.empty() ? local_dir(job.dest, job.remote_path) : job.dest / job.local_name;
}

fs::path local_dir(const fs::path& dest, const std::string& remote_path) {
    return dest / local::relative(remote_basename(remote_path));
}

std::string child_remote_path(const std::string& folder, const std::string& rel) {
    return folder.empty() || folder.ends_with('/') ? folder + rel : folder + "/" + rel;
}

void discard_partials(const std::string& remote_path, const fs::path& dir, const Tree& tree,
                      std::string_view rclone_remote) {
    const auto reserved = reserved_names(dir, tree);
    for (const auto& f : tree.files) {
        if (f.big && f.status != State::done) {
            fetch::discard(target_of(dir, f), child_remote_path(remote_path, f.path), reserved, rclone_remote);
        }
    }
}

std::expected<void, std::string> list(const Job& job, Tree& tree, std::mutex& m, std::stop_token stop) {
    {
        std::lock_guard lock(m);
        if (tree.listed) return {};
    }
    const auto& ctx = *job.ctx;
    std::vector<std::string> argv{ctx.config.rclone, "lsjson", "-R", "--files-only",
                                  remote_spec(ctx.settings.rclone_remote, job.remote_path)};
    if (ctx.config.rclone_config) argv.insert(argv.end(), {"--config", util::path_to_utf8(*ctx.config.rclone_config)});
    const auto flags = rclone_flags::connection(ctx.settings.common_flags);
    argv.insert(argv.end(), flags.begin(), flags.end());
    const auto extra = rclone_flags::connection(job.extra);
    argv.insert(argv.end(), extra.begin(), extra.end());
    // Its walk's connections come from the pool, as the downloads' do (review G191); without one
    // (--rclone), rclone's own.
    std::optional<fetch::Listing> walk;
    if (job.connections != nullptr) {
        walk.emplace(*job.connections, job.server, stop);
        if (stop.stop_requested()) return {};
        const auto walk_flags = walk->flags();
        argv.insert(argv.end(), walk_flags.begin(), walk_flags.end());
    }
    if (job.on_command) job.on_command(argv);
    proc::RunOptions quiet;
    quiet.detached = true;
    quiet.stop = stop;
    auto run = proc::run_capture(argv, quiet);
    if (stop.stop_requested()) return {};
    if (!run) return util::fail(run.error());
    if (run->exit_code != 0) return util::fail("cannot list the folder: " + server_ops::clean_rclone_error(run->err));
    auto listing = parse_listing(run->out);
    if (!listing) return util::fail(listing.error());
    {
        std::lock_guard lock(m);
        tree.files.clear();
        tree.files.reserve(listing->size());
        for (auto& f : *listing) tree.files.push_back(make_file(std::move(f.path), f.size, std::move(f.modtime)));
        settle_names(tree.files);
        // Without long paths, a file past what Windows takes here: grab couldn't see it on disk
        // or keep a partial download beside it, so it fails saying why (review G19).
        const auto dir = job_dir(job);
        for (auto& f : tree.files) {
            if (auto why = util::too_long(target_of(dir, f), f.big ? std::string_view(".2.grabpart.json").size() : 0)) {
                f.status = State::failed;
                f.error = std::move(*why);
            }
        }
        tree.listed = true;
    }
    notify(job, Event::checkpoint);
    return {};
}

void mark_existing(const Job& job, Tree& tree, std::mutex& m) {
    struct Candidate {
        std::size_t index;
        std::string path;
        std::uint64_t size;
        std::string modtime;
        bool big;
        fs::path local;
    };
    std::vector<Candidate> queued;
    const auto dir = job_dir(job);
    fetch::Reserved reserved;
    {
        std::lock_guard lock(m);
        for (std::size_t i = 0; i < tree.files.size(); ++i) {
            const auto& f = tree.files[i];
            if (f.status == State::queued) queued.push_back({i, f.path, f.size, f.modtime, f.big, target_of(dir, f)});
        }
        reserved = reserved_names(dir, tree);
    }
    // Looked at on disk without the lock: a big folder means many file system calls.
    std::vector<std::pair<std::size_t, std::optional<std::uint64_t>>> found; // done (nullopt) or partial bytes
    for (const auto& c : queued) {
        const auto partial = c.big ? fetch::saved_progress(c.local, child_remote_path(job.remote_path, c.path), reserved,
                                                            job.ctx->settings.rclone_remote)
                                   : std::nullopt;
        if (partial) {
            found.emplace_back(c.index, partial->first);
        } else if (fetch::up_to_date(c.local, c.size, c.modtime)) {
            found.emplace_back(c.index, std::nullopt);
        }
    }
    {
        std::lock_guard lock(m);
        for (const auto& [i, partial_bytes] : found) {
            auto& f = tree.files[i];
            if (f.status != State::queued) continue;
            if (partial_bytes) {
                f.bytes = *partial_bytes;
            } else {
                f.status = State::done;
                f.bytes = f.size;
            }
        }
    }
    notify(job, Event::changed);
}

std::vector<Conflict> conflicts(const Job& job, Tree& tree, std::mutex& m) {
    const auto dir = job_dir(job);
    std::vector<Conflict> queued;
    {
        std::lock_guard lock(m);
        for (const auto& f : tree.files) {
            if (f.status == State::queued) queued.push_back({f.path, target_of(dir, f), f.size, f.modtime});
        }
    }
    std::vector<Conflict> found; // looked at without the lock
    for (auto& c : queued) {
        if (existing::differs(c.local, c.size, util::parse_rfc3339(c.modtime))) found.push_back(std::move(c));
    }
    return found;
}

void apply(existing::Choice choice, const std::vector<Conflict>& found, const fs::path& dir, Tree& tree) {
    if (choice == existing::Choice::replace || choice == existing::Choice::ask) return;
    std::unordered_set<std::string> paths;
    for (const auto& c : found) paths.insert(c.path);
    const auto reserved = reserved_names(dir, tree);
    std::unordered_set<fs::path::string_type> taken; // second copies given out here
    for (auto& f : tree.files) {
        if (f.status != State::queued || !paths.contains(f.path)) continue;
        if (choice == existing::Choice::skip) {
            f.status = State::skipped;
            continue;
        }
        const auto second = existing::second_copy(target_of(dir, f), [&](const fs::path& p) {
            return reserved(p) || taken.contains(local::key(p));
        });
        if (!second) {
            f.status = State::failed;
            f.error = "no free name for a second copy";
            continue;
        }
        taken.insert(local::key(*second));
        f.local = second->lexically_relative(dir);
        f.big = true; // grab writes it under that name
    }
}

void run(const Job& job, Tree& tree, std::mutex& m, std::stop_token stop) {
    // A folder whose name rclone can't be told (rclone_encoding.hpp: a "\" in the server's name,
    // or a ‛ in the Save-to folder's): grab fetches every file itself rather than letting the batch
    // write somewhere else (G80).
    if (!rclone_enc::local_path(job_dir(job))) {
        std::lock_guard lock(m);
        for (auto& f : tree.files) f.big = true;
    }
    job.connections->join(job.server, job.id, job.weight != nullptr ? job.weight->load() : 2);
    while (!stop.stop_requested()) {
        // (not "small": windows.h defines that as a macro)
        std::vector<std::size_t> small_files;
        bool big_waiting = false;
        {
            std::lock_guard lock(m);
            for (std::size_t i = 0; i < tree.files.size(); ++i) {
                const auto& f = tree.files[i];
                if (f.status != State::queued) continue;
                if (f.big) big_waiting = true;
                else small_files.push_back(i);
            }
        }
        if (!small_files.empty()) run_batch(job, tree, m, small_files, stop);
        else if (big_waiting) run_big(job, tree, m, stop);
        else break;
        notify(job, Event::checkpoint);
    }
    job.connections->leave(job.server, job.id);

    {
        std::lock_guard lock(m);
        tree.connections = 0;
        for (auto& f : tree.files) {
            if (f.status != State::running) continue;
            f.status = State::queued; // stopped with the folder: picked up again on the next run
            f.speed = 0;
            f.eta.reset();
        }
    }
    notify(job, Event::changed);
}

std::string Checked::summary() const {
    if (!error.empty()) return "not verified: " + error;
    return verify::summary(report, files, true);
}

Checked verify_files(const Job& job, Tree& tree, std::mutex& m, std::stop_token stop) {
    Checked c;
    verify::Request r;
    {
        std::lock_guard lock(m);
        for (const auto& f : tree.files) {
            if (f.status != State::done || !f.fetched || f.verified) continue;
            r.paths.push_back(f.path);
            if (!f.local.empty()) r.renamed.emplace(f.path, f.local);
        }
    }
    if (r.paths.empty()) return c;
    c.files = r.paths.size();
    r.ctx = job.ctx;
    r.remote_dir = job.remote_path;
    r.local_dir = job_dir(job);
    r.extra = job.extra;
    r.connections = job.connections;
    r.server = job.server;
    r.id = job.id;
    r.many = true;
    r.on_command = job.on_command;
    auto report = verify::run(r, stop);
    if (!report) {
        c.error = report.error();
        return c;
    }
    c.report = std::move(*report);
    {
        std::lock_guard lock(m);
        const std::unordered_set<std::string> differ(c.report.differ.begin(), c.report.differ.end());
        const std::unordered_set<std::string> same(c.report.same.begin(), c.report.same.end());
        for (auto& f : tree.files) {
            if (f.status != State::done || !f.fetched || f.verified) continue;
            if (differ.contains(f.path)) {
                std::error_code ec;
                fs::remove(target_of(r.local_dir, f), ec); // not the server's file
                f.status = State::failed;
                f.error = "differs from the server's copy; deleted";
                f.fetched = false;
                f.bytes = 0;
            } else if (same.contains(f.path)) {
                f.verified = true;
            }
        }
    }
    notify(job, Event::changed);
    return c;
}

} // namespace grab::folder
