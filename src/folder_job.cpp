#include "folder_job.hpp"

#include "cli.hpp"
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

// The queued small files in one rclone copy: its connection pool is reused for every file,
// where a new SSH login per small file would dominate. Pausing or skipping one of them
// restarts the batch without it.
void run_batch(const Job& job, Tree& tree, std::mutex& m, const std::vector<std::string>& paths,
               const std::stop_token& stop) {
    const int reserved = job.connections->reserve(job.server, job.id, stop);
    if (reserved == 0) return; // stopped while waiting
    const auto list = fs::temp_directory_path() / std::format("grab-files-{}-{}.txt", process_id(), job.id);
    std::unordered_map<std::string, std::size_t> index; // path -> file
    if (auto written = util::write_file_atomic(list, files_from_text(paths)); !written) {
        job.connections->release(job.server, job.id, reserved);
        std::lock_guard lock(m);
        for (std::size_t i = 0; i < tree.files.size(); ++i) index.emplace(tree.files[i].path, i);
        for (const auto& p : paths) {
            auto& f = tree.files[index[p]];
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
        for (const auto& p : paths) {
            auto& f = tree.files[index[p]];
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
    std::vector<std::string> extra{"--files-from-raw", util::path_to_utf8(list), "--transfers",
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
    const auto dir = local_dir(job.dest, job.remote_path);
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
        });
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
    for (const auto& p : paths) {
        auto& f = tree.files[index[p]];
        const bool was_in_batch = f.in_batch;
        f.in_batch = false;
        if (!was_in_batch || (f.status != State::running && f.status != State::queued)) continue; // done, paused, skipped
        f.speed = 0;
        f.eta.reset();
        if (finished) {
            const auto local = target_of(dir, f);
            if (fs::file_size(local, ec) == f.size && !ec) {
                f.status = State::done;
                f.fetched = true;
                f.bytes = f.size;
            } else {
                f.status = State::failed;
                f.error = "missing after the copy";
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
    const auto dir = local_dir(job.dest, job.remote_path);
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
            if (discard) fetch::discard(d.target, d.source.path, d.reserved);
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
    // A device name (CON, nul.txt...) anywhere in its path: grab fetches it itself, since rclone
    // would write it through a \\?\ path most programs can't open, under another name than grab's.
    f.big = is_big(size) || local::has_device(f.path);
    return f;
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

fs::path local_dir(const fs::path& dest, const std::string& remote_path) {
    return dest / local::relative(remote_basename(remote_path));
}

std::string child_remote_path(const std::string& folder, const std::string& rel) {
    return folder.empty() || folder.ends_with('/') ? folder + rel : folder + "/" + rel;
}

void discard_partials(const std::string& remote_path, const fs::path& dir, const Tree& tree) {
    const auto reserved = reserved_names(dir, tree);
    for (const auto& f : tree.files) {
        if (f.big && f.status != State::done) {
            fetch::discard(target_of(dir, f), child_remote_path(remote_path, f.path), reserved);
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
    // Its walk's connections come from the pool, as the downloads' do (review G191).
    const fetch::Listing walk(*job.connections, job.server, stop);
    if (stop.stop_requested()) return {};
    const auto walk_flags = walk.flags();
    argv.insert(argv.end(), walk_flags.begin(), walk_flags.end());
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
    const auto dir = local_dir(job.dest, job.remote_path);
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
        const auto partial = c.big ? fetch::saved_progress(c.local, child_remote_path(job.remote_path, c.path), reserved)
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
    const auto dir = local_dir(job.dest, job.remote_path);
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
    if (!rclone_enc::local_path(local_dir(job.dest, job.remote_path))) {
        std::lock_guard lock(m);
        for (auto& f : tree.files) f.big = true;
    }
    job.connections->join(job.server, job.id, job.weight != nullptr ? job.weight->load() : 2);
    while (!stop.stop_requested()) {
        // (not "small": windows.h defines that as a macro)
        std::vector<std::string> small_files;
        bool big_waiting = false;
        {
            std::lock_guard lock(m);
            for (const auto& f : tree.files) {
                if (f.status != State::queued) continue;
                if (f.big) big_waiting = true;
                else small_files.push_back(f.path);
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
    r.local_dir = local_dir(job.dest, job.remote_path);
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
