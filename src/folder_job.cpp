#include "folder_job.hpp"

#include "cli.hpp"
#include "existing.hpp"
#include "local_name.hpp"
#include "process.hpp"
#include "rclone.hpp"
#include "rclone_encoding.hpp"
#include "rclone_flags.hpp"
#include "server_ops.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"
#include "ssh_walk.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <format>
#include <functional>
#include <map>
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

// How many of the folder's files may download at once now (Job::files_at_once).
int allowed(const Job& job) { return job.files_at_once ? std::max(1, job.files_at_once()) : 4; }

// The queued small files (`batch_files`: their indexes) in one rclone copy: its connection pool
// is reused for every file, where a new SSH login per small file would dominate. As many files
// at once as the folder may download, each on a connection of its own; it starts again with
// another number when that changes. Pausing or skipping one of them restarts the batch without it.
void run_batch(const Job& job, Tree& tree, std::mutex& m, const std::vector<std::size_t>& batch_files,
               const std::stop_token& stop) {
    const int wanted = allowed(job);
    const int reserved = job.connections->reserve(job.server, job.id, stop, wanted);
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
        tree.files_at_once = reserved;
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
            // again with the new number; so does a change of how many files it may download at
            // once. Only the files in flight start over.
            if (const auto now = std::chrono::steady_clock::now(); now - last_check >= std::chrono::seconds(3)) {
                last_check = now;
                if (job.connections->unbalanced(job.server, job.id, reserved, wanted) || allowed(job) != wanted) {
                    batch.request_stop();
                }
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
    tree.files_at_once = 0;
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

// Waits `d` unless `stop` comes first.
void pause_for(std::chrono::milliseconds d, const std::stop_token& stop) {
    std::mutex wait_m;
    std::condition_variable_any wait_cv;
    std::unique_lock lock(wait_m);
    wait_cv.wait_for(lock, stop, d, [] { return false; });
}

// What one worker's turn on a connection came to.
struct SmallRun {
    std::size_t landed = 0;            // files put in place (or failed for their own reasons)
    bool worked = false;               // data came: the pool counts the connection as working
    bool yielded = false;              // handed its connection to another download, which released it
    std::optional<std::string> broken; // the connection broke: why
};

// Small files on one connection (run_small_grab): up to 32 in flight, each OPEN, then its READs
// and an FSTAT (the size and time now, which may have changed since the listing), then CLOSE, all
// pipelined; 8 MiB of READs in flight at most. Under a speed limit one file and one READ at a
// time, its tokens taken before it, so the replies are never left unread through a token wait.
// Files left half-way (a stop, a broken connection) go back to the queue.
SmallRun small_on(const Job& job, Tree& tree, std::mutex& m, sftp::Client& c, const fs::path& dir,
                  const std::stop_token& stop) {
    const bool limited = job.limiter != nullptr && job.limiter->rate() > 0;
    const std::size_t max_files = limited ? 1 : 32;
    constexpr std::uint64_t max_bytes = std::uint64_t{8} << 20;
    const bool replace = job.existing == existing::Choice::replace;
    struct Slot {
        std::size_t index = 0;
        std::string raw;
        fs::path target;
        std::uint64_t size = 0;
        std::string modtime;
        std::string handle;
        std::string data;
        std::uint64_t asked = 0;
        std::uint64_t got = 0;
        int reads = 0;   // READs on their way
        int pending = 0; // requests of any kind on their way
        bool opened = false;
        bool stated = false;
        bool done = false; // landed or failed: the slot goes once its replies are in
        std::optional<std::string> error;
    };
    std::map<int, Slot> slots;
    int next_id = 0;
    int reads_in_flight = 0;
    std::uint64_t bytes_in_flight = 0;
    bool taking = true;
    SmallRun run;
    auto last_progress = std::chrono::steady_clock::now();
    const int weight = job.weight != nullptr ? job.weight->load() : 2;

    // Back to the queue: the slots' files not landed yet.
    auto requeue = [&] {
        std::lock_guard lock(m);
        for (auto& [id, s] : slots) {
            if (s.done) continue;
            auto& f = tree.files[s.index];
            if (f.status != State::running) continue;
            f.status = State::queued;
            f.bytes = 0;
            f.speed = 0;
        }
    };
    std::function<void(int, std::uint64_t, std::uint32_t)> send_read = [&](int id, std::uint64_t at, std::uint32_t n) {
        Slot& s = slots.at(id);
        ++s.reads;
        ++s.pending;
        ++reads_in_flight;
        bytes_in_flight += n;
        c.read(s.handle, at, n, [&, id, at, n](std::expected<std::string_view, sftp::Status> r) {
            Slot& s = slots.at(id);
            --s.reads;
            --s.pending;
            --reads_in_flight;
            bytes_in_flight -= n;
            if (!r) {
                // The end of the file where the FSTAT says it ends is no error.
                if (r.error().code != sftp::Code::eof || at < s.size) {
                    if (!s.error) s.error = r.error().code == sftp::Code::eof
                                                ? "the server sent less data than expected; was the file changed?"
                                                : sftp::describe(r.error());
                }
                return;
            }
            const std::uint64_t room = s.size > at ? std::min<std::uint64_t>(r->size(), s.size - at) : 0;
            std::memcpy(s.data.data() + at, r->data(), static_cast<std::size_t>(room));
            s.got += room;
            if (!run.worked) {
                run.worked = true;
                job.connections->working(job.server, job.id);
            }
            if (r->size() < n && at + r->size() < s.size) {
                send_read(id, at + r->size(), static_cast<std::uint32_t>(n - r->size())); // the rest, exactly
            }
            {
                std::lock_guard lock(m);
                auto& f = tree.files[s.index];
                if (f.status == State::running) f.bytes = s.got;
            }
            if (const auto now = std::chrono::steady_clock::now(); now - last_progress >= std::chrono::milliseconds(100)) {
                last_progress = now;
                notify(job, Event::progress);
            }
        });
    };
    // A file whose bytes are all in, or that failed: closed on the server, put in place or failed.
    auto finish = [&](int id) {
        Slot& s = slots.at(id);
        s.done = true;
        if (s.opened) {
            ++s.pending;
            c.close(s.handle, [&, id](std::expected<void, sftp::Status>) { --slots.at(id).pending; });
        }
        std::string why = s.error.value_or("");
        bool present = false;
        {
            std::lock_guard lock(m);
            if (tree.files[s.index].status != State::running) return; // paused or skipped meanwhile
        }
        if (why.empty()) {
            std::error_code ec;
            if (!replace && fs::exists(fs::symlink_status(s.target, ec))) {
                // Conflicts were answered before; one that appeared since stays, as with rclone.
                if (fetch::up_to_date(s.target, s.size, s.modtime)) present = true;
                else why = "a different file is already at " + util::path_to_utf8(s.target);
            } else if (auto placed = util::place_file(s.target, s.data, util::parse_rfc3339(s.modtime)); !placed) {
                why = placed.error();
            }
        }
        s.data = std::string(); // its memory back now, not when its CLOSE comes
        {
            std::lock_guard lock(m);
            auto& f = tree.files[s.index];
            f.speed = 0;
            f.eta.reset();
            if (why.empty()) {
                f.status = State::done;
                f.fetched = f.fetched || !present;
                f.bytes = f.size;
            } else {
                f.status = State::failed;
                f.error = std::move(why);
            }
        }
        ++run.landed;
        notify(job, Event::progress); // many a second in a folder of small files
        // A download that just joined this server gets its share now: no more files for this one.
        if (taking && job.connections->yield_if_over_share(job.server, job.id)) {
            run.yielded = true;
            taking = false;
        }
    };

    try {
        for (;;) {
            // New files, up to the slots this connection keeps.
            while (taking && slots.size() < max_files && !stop.stop_requested()) {
                Slot s;
                std::string path;
                {
                    std::lock_guard lock(m);
                    auto it = std::ranges::find_if(tree.files, [](const File& f) { return !f.big && f.status == State::queued; });
                    if (it == tree.files.end()) {
                        taking = false;
                        break;
                    }
                    it->status = State::running;
                    it->bytes = 0;
                    it->error.clear();
                    s.index = static_cast<std::size_t>(it - tree.files.begin());
                    s.size = it->size;
                    s.modtime = it->modtime;
                    s.target = target_of(dir, *it);
                    path = it->path;
                }
                s.raw = rclone_enc::raw_path(child_remote_path(job.remote_path, path));
                s.data.resize(static_cast<std::size_t>(s.size));
                const int id = next_id++;
                Slot& slot = slots.emplace(id, std::move(s)).first->second;
                ++slot.pending;
                c.open(slot.raw, [&, id](std::expected<std::string, sftp::Status> h) {
                    Slot& s = slots.at(id);
                    --s.pending;
                    if (!h) {
                        s.error = sftp::describe(h.error());
                        return;
                    }
                    s.handle = std::move(*h);
                    s.opened = true;
                    ++s.pending;
                    c.fstat(s.handle, [&, id](std::expected<sftp::Attrs, sftp::Status> a) {
                        Slot& s = slots.at(id);
                        --s.pending;
                        s.stated = true;
                        if (!a) return; // the listing's size and time stand
                        if (a->size && *a->size != s.size) {
                            s.size = *a->size;
                            s.data.resize(static_cast<std::size_t>(s.size));
                        }
                        if (a->mtime) s.modtime = std::format("{:%FT%TZ}", std::chrono::sys_seconds{std::chrono::seconds{*a->mtime}});
                    });
                });
                notify(job, Event::progress);
            }
            // READs for what the open files still lack.
            for (auto& [id, s] : slots) {
                while (s.opened && !s.error && !s.done && s.asked < s.size && !stop.stop_requested()) {
                    const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(sftp::max_read, s.size - s.asked));
                    if (limited ? reads_in_flight >= 1 : reads_in_flight > 0 && bytes_in_flight + n > max_bytes) break;
                    if (limited && !job.limiter->acquire(n, stop, job.id, weight)) break;
                    send_read(id, s.asked, n);
                    s.asked += n;
                }
            }
            if (stop.stop_requested()) break;
            // Files complete or failed: put in place; slots whose replies are all in go.
            for (auto it = slots.begin(); it != slots.end();) {
                Slot& s = it->second;
                const bool complete = s.opened && s.stated && s.asked >= s.size && s.reads == 0;
                if (!s.done && (s.error || complete)) finish(it->first);
                it = s.done && s.pending == 0 ? slots.erase(it) : std::next(it);
            }
            if (slots.empty() && !taking) break;
            if (c.pending() == 0) {
                if (slots.empty()) continue; // take more
                requeue(); // nothing on its way and nothing to send: should not happen; never spin
                return run;
            }
            c.pump();
        }
    } catch (const sftp::Broken& e) {
        requeue();
        run.broken = e.what();
        return run;
    }
    if (stop.stop_requested()) requeue();
    return run;
}

// The queued small files (under small_limit) through grab's engine: as many workers as the
// server's budget, each holding one of the folder's connections while small files are left, many
// files in flight on each (small_on). They don't count against At once; the folder's big files,
// fetched beside them (run_big), do.
void run_small_grab(const Job& job, Tree& tree, std::mutex& m, const std::stop_token& stop) {
    const auto dir = job_dir(job);
    auto any_queued = [&] {
        std::lock_guard lock(m);
        return std::ranges::any_of(tree.files, [](const File& f) { return !f.big && f.status == State::queued; });
    };
    auto fail_queued = [&](const std::string& why) {
        {
            std::lock_guard lock(m);
            for (auto& f : tree.files) {
                if (f.big || f.status != State::queued) continue;
                f.status = State::failed;
                f.error = why;
            }
        }
        notify(job, Event::changed);
    };
    auto worker = [&] {
        int failures = 0; // in a row, nothing landing
        for (;;) {
            if (stop.stop_requested() || !any_queued()) return;
            if (!job.connections->acquire(job.server, job.id, stop, false)) return;
            std::optional<ssh::Pool::Lease> lease;
            std::string why;
            auto kind = budget::Failure::other;
            try {
                lease = job.pool->acquire(stop);
            } catch (const ssh::Error& e) {
                why = e.what();
                kind = e.kind;
            }
            if (!lease) {
                if (!why.empty() && kind == budget::Failure::refused) job.connections->refused(job.server);
                job.connections->release(job.server, job.id);
                if (why.empty()) return; // stopped
                if (kind == budget::Failure::fatal || ++failures > fetch::max_retries) return fail_queued(why);
                // A full server is asked again 1, 2, 4, 8, then 15 s apart (as fetch does, G9).
                pause_for(std::min(std::chrono::seconds(1 << std::min(failures - 1, 4)), std::chrono::seconds(15)), stop);
                continue;
            }
            {
                std::lock_guard lock(m);
                tree.connections = job.connections->active(job.server, job.id);
            }
            SmallRun run;
            try {
                run = small_on(job, tree, m, (*lease)->sftp(), dir, stop);
            } catch (const sftp::Unavailable& e) { // the server has no SFTP: no use trying again
                lease.reset();
                job.connections->release(job.server, job.id);
                return fail_queued(e.what());
            } catch (const sftp::Broken& e) {
                run.broken = e.what(); // SFTP didn't start on it
            }
            if (stop.stop_requested() || run.broken) lease->broken(); // replies still on their way
            lease.reset();
            if (!run.yielded) job.connections->release(job.server, job.id, 1, run.worked);
            if (run.landed > 0) failures = 0;
            if (run.broken && run.landed == 0 && ++failures > fetch::max_retries) return fail_queued(*run.broken);
            if (run.broken) pause_for(std::chrono::seconds(2), stop);
        }
    };
    std::vector<std::jthread> workers;
    for (int k = 0; k < std::max(1, job.connections->budget(job.server)); ++k) workers.emplace_back(worker);
}

// The queued big files, each resumable like a single-file download, as many at a time as the
// folder may download ("At once": Job::files_at_once, asked again as each file ends). They share
// the folder's connections (same pool id) and split ranges to keep them busy.
void run_big(const Job& job, Tree& tree, std::mutex& m, const std::stop_token& stop) {
    constexpr int most = 8; // "At once" goes up to 8
    const auto dir = job_dir(job);
    fetch::Reserved reserved;
    {
        std::lock_guard lock(m);
        reserved = reserved_names(dir, tree);
    }
    auto flags = rclone_flags::connection(job.ctx->settings.common_flags);
    const auto extra = rclone_flags::connection(job.extra);
    flags.insert(flags.end(), extra.begin(), extra.end());
    // Its big files downloading now: they split the folder's connections evenly, so one that
    // starts later gets its part instead of waiting for the first ones to end.
    std::atomic<int> files_running{0};
    auto even_part = [&] {
        return std::max(1, job.connections->share(job.server, job.id) / std::max(1, files_running.load()));
    };
    auto worker = [&] {
        for (;;) {
            if (stop.stop_requested()) return;
            const int may = allowed(job); // without the lock: the window's answer takes its own
            std::size_t i = 0;
            std::stop_source file_stop;
            std::string path;
            fs::path target;
            bool wait = false;
            {
                std::lock_guard lock(m);
                auto it = std::ranges::find_if(tree.files, [](const File& f) { return f.big && f.status == State::queued; });
                if (it == tree.files.end()) return;
                wait = tree.files_at_once >= may;
            }
            if (wait) { // as many as it may are running: another starts when one ends or more are allowed
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            {
                std::lock_guard lock(m);
                auto it = std::ranges::find_if(tree.files, [](const File& f) { return f.big && f.status == State::queued; });
                if (it == tree.files.end()) return;
                if (tree.files_at_once >= may) continue; // another worker took the place
                ++tree.files_at_once;
                ++files_running;
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
            d.streams_cap = even_part;
            d.on_command = job.on_command;
            d.pool = job.pool;
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
                --tree.files_at_once;
                --files_running;
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
    for (int k = 0; k < most; ++k) workers.emplace_back(worker);
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

void set_big(std::vector<File>& files, bool grab_engine) {
    if (grab_engine) {
        for (auto& f : files) f.big = f.size >= small_limit;
        return;
    }
    std::vector<std::string> paths;
    paths.reserve(files.size());
    for (const auto& f : files) paths.push_back(f.path);
    const auto clash = local::clashing(paths);
    for (auto& f : files) {
        f.big = is_big(f.size) || local::differs_from_rclone(f.path) || !f.local.empty() || clash.contains(f.path);
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

namespace {

// A folder's listing into its tree: its files (settled names, which take fetch::run), and those
// whose path Windows can't take here failed saying why.
void take_listing(const Job& job, Tree& tree, std::mutex& m, std::vector<FolderFile> listing,
                  std::vector<std::string> unreadable) {
    {
        std::lock_guard lock(m);
        tree.files.clear();
        tree.files.reserve(listing.size());
        for (auto& f : listing) tree.files.push_back(make_file(std::move(f.path), f.size, std::move(f.modtime)));
        settle_names(tree.files);
        set_big(tree.files, job.pool != nullptr);
        // Without long paths, a file past what Windows takes here: grab couldn't see it on disk
        // or keep a partial download beside it, so it fails saying why (review G19).
        const auto dir = job_dir(job);
        for (auto& f : tree.files) {
            if (auto why = util::too_long(target_of(dir, f), f.big ? std::string_view(".2.grabpart.json").size() : 0)) {
                f.status = State::failed;
                f.error = std::move(*why);
            }
        }
        tree.unreadable = std::move(unreadable);
        tree.listed = true;
    }
    notify(job, Event::checkpoint);
}

// The listing through grab's engine: an SFTP walk on the folder's pooled connections, as many
// as a listing may take from the server's budget (fetch::Listing, review G191).
std::expected<void, std::string> list_grab(const Job& job, Tree& tree, std::mutex& m, const std::stop_token& stop) {
    fetch::Listing seat(*job.connections, job.server, stop);
    if (stop.stop_requested()) return {};
    if (job.on_command) job.on_command({"sftp", "walk", job.remote_path});
    auto walked = ssh::walk(*job.pool, job.remote_path, ssh::WalkOptions{.connections = std::max(1, seat.connections())},
                            stop);
    if (stop.stop_requested()) return {};
    if (!walked) return util::fail("cannot list the folder: " + walked.error());
    std::vector<FolderFile> listing;
    listing.reserve(walked->entries.size());
    for (auto& e : walked->entries) listing.push_back(FolderFile{std::move(e.path), e.size, std::move(e.modtime)});
    take_listing(job, tree, m, std::move(listing), std::move(walked->unreadable));
    return {};
}

} // namespace

std::expected<void, std::string> list(const Job& job, Tree& tree, std::mutex& m, std::stop_token stop) {
    {
        std::lock_guard lock(m);
        if (tree.listed) return {};
    }
    if (job.pool) return list_grab(job, tree, m, stop);
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
    take_listing(job, tree, m, std::move(*listing), {});
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
        set_big(tree.files, job.pool != nullptr); // which ones may have a partial download
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
    {
        std::lock_guard lock(m);
        set_big(tree.files, job.pool != nullptr);
        // A folder whose name rclone can't be told (rclone_encoding.hpp: a "\" in the server's
        // name, or a ‛ in the Save-to folder's): grab fetches every file itself rather than letting
        // the batch write somewhere else (G80).
        if (!job.pool && !rclone_enc::local_path(job_dir(job))) {
            for (auto& f : tree.files) f.big = true;
        }
    }
    job.connections->join(job.server, job.id, job.weight != nullptr ? job.weight->load() : 2);
    // grab's engine: the small files on every free connection beside the big ones under At once,
    // a pass at a time until none is queued (a paused one resumed, a failed one retried).
    while (job.pool && !stop.stop_requested()) {
        bool small_left = false;
        bool big_left = false;
        {
            std::lock_guard lock(m);
            for (const auto& f : tree.files) {
                if (f.status != State::queued) continue;
                (f.big ? big_left : small_left) = true;
            }
        }
        if (!small_left && !big_left) break;
        {
            std::jthread small_files;
            if (small_left) small_files = std::jthread([&] { run_small_grab(job, tree, m, stop); });
            if (big_left) run_big(job, tree, m, stop);
        }
        notify(job, Event::checkpoint);
    }
    while (!job.pool && !stop.stop_requested()) {
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
            // A name that isn't UTF-8 (fetched by grab's engine) can't reach rclone's check, which
            // would call it missing and so have the good copy deleted: left unchecked until the
            // check is grab's own (RCLONE-REPLACEMENT.md, phase 5).
            if (!rclone_enc::valid_utf8(rclone_enc::raw_path(f.path))) continue;
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
