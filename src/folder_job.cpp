#include "folder_job.hpp"

#include "cli.hpp"
#include "local_name.hpp"
#include "process.hpp"
#include "rclone.hpp"
#include "server_ops.hpp"
#include "util.hpp"

#include <algorithm>
#include <format>
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
    (void)server_ops::write_text(list, files_from_text(paths));
    const std::uint64_t limit = job.batch_limit ? job.batch_limit() : 0;

    std::unordered_map<std::string, std::size_t> index; // path -> file
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
    extra.insert(extra.end(), job.extra.begin(), job.extra.end());
    std::uint64_t charged = 0;
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
    job.connections->release(job.server, job.id, reserved);

    const auto dir = local_dir(job.dest, job.remote_path);
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
            const auto local = dir / local::relative(f.path);
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
    auto flags = engine::without_console_progress(job.ctx->settings.common_flags);
    flags.insert(flags.end(), job.extra.begin(), job.extra.end());
    auto worker = [&] {
        for (;;) {
            if (stop.stop_requested()) return;
            std::size_t i = 0;
            std::stop_source file_stop;
            std::string path;
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
            }
            notify(job, Event::changed);
            std::stop_callback forward(stop, [&file_stop] { file_stop.request_stop(); });
            fetch::Download d;
            d.source = fetch::Source{job.ctx->config.rclone, job.ctx->config.rclone_config,
                                     job.ctx->settings.rclone_remote, child_remote_path(job.remote_path, path), flags};
            d.server = job.server;
            d.id = job.id;
            d.target = dir / local::relative(path);
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
            if (discard) fetch::discard(d.target);
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

std::optional<fs::path> skip_file(Tree& tree, const fs::path& dir, const std::string& path) {
    std::optional<fs::path> discard;
    for (auto& f : tree.files) {
        if (f.path != path) continue;
        if (f.status == State::queued || f.status == State::paused || f.status == State::failed) {
            if (f.status == State::queued && f.in_batch) tree.batch_stop.request_stop(); // rclone would still fetch it
            f.status = State::skipped;
            if (f.big) discard = dir / local::relative(f.path);
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

fs::path local_dir(const fs::path& dest, const std::string& remote_path) {
    return dest / local::relative(remote_basename(remote_path));
}

std::string child_remote_path(const std::string& folder, const std::string& rel) {
    return folder.empty() || folder.ends_with('/') ? folder + rel : folder + "/" + rel;
}

void discard_partials(const fs::path& dir, const Tree& tree) {
    for (const auto& f : tree.files) {
        if (f.big && f.status != State::done) fetch::discard(dir / local::relative(f.path));
    }
    engine::remove_partials(dir, Mode::folder); // rclone's own .partial files
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
    const auto flags = engine::without_console_progress(ctx.settings.common_flags);
    argv.insert(argv.end(), flags.begin(), flags.end());
    argv.insert(argv.end(), job.extra.begin(), job.extra.end());
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
    };
    std::vector<Candidate> queued;
    {
        std::lock_guard lock(m);
        for (std::size_t i = 0; i < tree.files.size(); ++i) {
            const auto& f = tree.files[i];
            if (f.status == State::queued) queued.push_back({i, f.path, f.size, f.modtime, f.big});
        }
    }
    // Looked at on disk without the lock: a big folder means many file system calls.
    const auto dir = local_dir(job.dest, job.remote_path);
    std::vector<std::pair<std::size_t, std::optional<std::uint64_t>>> found; // done (nullopt) or partial bytes
    for (const auto& c : queued) {
        const auto local = dir / local::relative(c.path);
        std::error_code ec;
        const bool partial = c.big && fs::exists(fetch::state_path(local), ec);
        if (!partial && fetch::up_to_date(local, c.size, c.modtime)) {
            found.emplace_back(c.index, std::nullopt);
        } else if (partial) {
            if (auto p = fetch::saved_progress(local)) found.emplace_back(c.index, p->first);
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

void run(const Job& job, Tree& tree, std::mutex& m, std::stop_token stop) {
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
            if (f.status == State::done && f.fetched && !f.verified) r.paths.push_back(f.path);
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
                fs::remove(r.local_dir / local::relative(f.path), ec); // not the server's file
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
