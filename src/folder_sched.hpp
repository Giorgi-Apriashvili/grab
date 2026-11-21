#pragma once

// How a folder's files are scheduled, the same for a folder download (folder_job.cpp) and a folder
// upload (upload_folder.cpp) (UPLOADS.md 5.6): its small files (under small_limit) go on every
// connection the server's budget gives the folder, many in flight on each (the caller's small_on),
// beside its big files, each moved as a single file is (the caller's big_file), as many at a time
// as "At once" says. Moved out of folder_job.cpp as it was: the folder downloads' tests
// (test_folder_grab.cpp) hold it.
//
// A job is a folder::Job or an upload::FolderJob: what this needs of it has the same name in both
// (connections, server, id, weight, files_at_once, on_event, on_small_start, pool).

#include "conn_budget.hpp"
#include "fetch.hpp"
#include "folder.hpp"
#include "folder_job.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace grab::folder::sched {

template <class Job> void notify(const Job& job, Event e) {
    if (job.on_event) job.on_event(e);
}

// How many of the folder's files may move at once now (Job::files_at_once).
template <class Job> int allowed(const Job& job) { return job.files_at_once ? std::max(1, job.files_at_once()) : 4; }

// Waits `d` unless `stop` comes first.
inline void pause_for(std::chrono::milliseconds d, const std::stop_token& stop) {
    std::mutex wait_m;
    std::condition_variable_any wait_cv;
    std::unique_lock lock(wait_m);
    wait_cv.wait_for(lock, stop, d, [] { return false; });
}

// Files under small_limit come too quickly for a speed of their own: their priority only orders
// them. Under a speed limit a small worker takes a normal file's part of the folder's turns,
// whatever its file's priority.
inline constexpr int small_weight = 2;
// The limiter's members of a folder (rate::flow_of; member 0 is a download as a whole): a big
// file's number is its index + 1, a small worker's has the top bit set.
inline constexpr std::uint32_t small_member = std::uint32_t{1} << 31;
inline std::uint32_t big_member(std::size_t index) { return static_cast<std::uint32_t>(index) + 1; }

// What the folder's running files share (DOWNLOADS-UI.md 7), under the caller's mutex. The
// connections its small workers don't claim (Tree::small_claim) go to its running big files by
// their priorities (connection_parts). The claim counts from before the big files start, not from
// when a worker holds a connection: counting only those held let big files started first take
// every connection when the small files' thread ran late (a busy machine), and keep each until its
// range ended. Under a speed limit the folder is one download in the limiter, at its own weight,
// and its files that read take its turns by their priorities (rate::FairQueue's parts), so its
// share against other downloads stays as it was, however many of its files run and whenever they
// start. A file that waits for a connection asks the limiter nothing, so those that read take all
// of the folder's turns.
struct Members {
    struct Big {
        std::size_t index = 0; // the file's
        int reading = 0;       // its streams reading now (fetch::Download::on_reading)
        bool all_in = false;   // every byte of it is in: it no longer waits for anything
    };
    std::vector<Big> big; // running big files, in the order they started

    // The running big files' priorities, in the order of `big`.
    [[nodiscard]] std::vector<int> big_weights(const Tree& tree) const {
        std::vector<int> weights;
        weights.reserve(big.size());
        for (const auto& b : big) weights.push_back(tree.files[b.index].weight);
        return weights;
    }
    Big* find(std::size_t index) {
        const auto it = std::ranges::find(big, index, &Big::index);
        return it != big.end() ? &*it : nullptr;
    }
};

// What one worker's turn on a connection came to.
struct SmallRun {
    std::size_t landed = 0;            // files put in place (or failed for their own reasons)
    bool worked = false;               // data came: the pool counts the connection as working
    bool yielded = false;              // handed its connection to another download, which released it
    bool cut = false;                  // a limit switched on or lowered below what it had on its way
    std::optional<std::string> broken; // the connection broke: why
};

// A big file being started (run_big): which, and what its single-file transfer is given: its part
// of the folder's connections, the hooks that show it waiting and give it its part of the folder's
// turns under a speed limit, its progress, and its own stop.
struct BigFile {
    std::size_t index = 0;
    std::uint32_t member = 0;
    std::function<int()> streams_cap;
    std::function<void(int delta, bool all_in)> on_reading;
    std::function<double()> flow_part;
    std::function<void(const fetch::Progress&)> on_progress;
    std::stop_token stop;
};

// What a big file came to.
struct BigEnd {
    enum class Kind { done, failed, stopped, skipped };
    Kind kind = Kind::failed;
    std::string error;    // failed; skipped: why it was kept as it was, when not the user's choice
    bool moved = false;   // done: by this transfer (not found there already)
    std::uint64_t size = 0; // done: the file as moved, which verification proves against
    std::string modtime;
    // Stopped by a skip: what is to be thrown away once the mutex is released.
    std::function<void()> discard;
    // Run under the mutex, after the file's state is set: the caller's own bookkeeping.
    std::function<void()> under_lock;
};

// The queued small files (under small_limit): `workers` of them (the server's budget), each
// holding one of the folder's connections while small files are left, many files in flight on
// each (small_on). They don't count against At once; the folder's big files, started beside them
// (run_big), do, and get the connections the small files leave: each worker's claim
// (Tree::small_claim) counts from before the big files start (the caller sets it) until it finds no
// small file left, so however late this thread runs, the big files wait while the small files
// claim them all (the window says "Waiting" while the claim lasts), and take them as the workers
// end. `small_on(member, client, stop, carried)` is one worker's turn on a connection (SmallRun),
// called from every worker's thread.
template <class Job, class SmallOn>
void run_small(const Job& job, Tree& tree, std::mutex& m, int workers, const std::stop_token& stop, SmallOn& small_on) {
    auto any_queued = [&] {
        std::lock_guard lock(m);
        return next_queued(tree, false).has_value();
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
    auto worker = [&](std::uint32_t k) {
        int failures = 0; // in a row, nothing landing
        // The connection it holds (Tree::small_connections); its claim on one (Tree::small_claim)
        // holds the big files back. A worker whose turn a new limit cut keeps counting while it
        // logs in again, so the folder's count of its small files' connections doesn't dip for
        // that moment.
        auto member = [&](int delta) {
            std::lock_guard lock(m);
            tree.small_connections += delta;
            if (delta > 0) tree.connections = job.connections->active(job.server, job.id);
        };
        // Its claim ends with it: the big files take its connection at their next look.
        auto unclaim = [&] {
            std::lock_guard lock(m);
            --tree.small_claim;
        };
        bool kept = false;
        // Its files a cut left running for its next turn (small_on); back to the queue when there
        // is none soon.
        std::vector<std::size_t> carried;
        auto requeue_carried = [&] {
            if (carried.empty()) return;
            {
                std::lock_guard lock(m);
                for (const std::size_t i : carried) {
                    auto& f = tree.files[i];
                    if (f.status != State::running) continue;
                    f.status = State::queued;
                    ++tree.requeued;
                }
            }
            carried.clear();
            notify(job, Event::changed);
        };
        struct Leave {
            bool& kept;
            decltype(member)& leave;
            decltype(requeue_carried)& requeue;
            decltype(unclaim)& unclaim;
            ~Leave() {
                requeue();
                if (std::exchange(kept, false)) leave(-1);
                unclaim();
            }
        } leave_at_end{.kept = kept, .leave = member, .requeue = requeue_carried, .unclaim = unclaim};
        for (;;) {
            if (stop.stop_requested() || (carried.empty() && !any_queued())) return;
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
                requeue_carried(); // another worker may take them meanwhile
                if (std::exchange(kept, false)) member(-1);
                if (!why.empty() && kind == budget::Failure::refused) job.connections->refused(job.server);
                job.connections->release(job.server, job.id);
                if (why.empty()) return; // stopped
                if (kind == budget::Failure::fatal || ++failures > fetch::max_retries) return fail_queued(why);
                // A full server is asked again 1, 2, 4, 8, then 15 s apart (as fetch does, G9).
                pause_for(std::min(std::chrono::seconds(1 << std::min(failures - 1, 4)), std::chrono::seconds(15)), stop);
                continue;
            }
            if (!std::exchange(kept, false)) member(+1);
            SmallRun run;
            try {
                run = small_on(small_member | k, (*lease)->sftp(), stop, carried);
            } catch (const sftp::Unavailable& e) { // the server has no SFTP: no use trying again
                requeue_carried();
                member(-1);
                lease.reset();
                job.connections->release(job.server, job.id);
                return fail_queued(e.what());
            } catch (const sftp::Broken& e) {
                run.broken = e.what(); // SFTP didn't start on it
            }
            kept = run.cut && !stop.stop_requested();
            if (!kept) {
                requeue_carried(); // a stop, or SFTP didn't start on it: none taken
                member(-1);
            }
            // Replies still on their way: dropped with the connection.
            if (stop.stop_requested() || run.broken || run.cut) lease->broken();
            lease.reset();
            if (!run.yielded) job.connections->release(job.server, job.id, 1, run.worked);
            if (run.landed > 0) failures = 0;
            if (run.cut) continue; // not a failure: its files go on over a new login
            if (run.broken && run.landed == 0 && ++failures > fetch::max_retries) return fail_queued(*run.broken);
            if (run.broken) pause_for(std::chrono::seconds(2), stop);
        }
    };
    if (job.on_small_start) job.on_small_start();
    std::vector<std::jthread> threads;
    for (int k = 0; k < workers; ++k) threads.emplace_back(worker, static_cast<std::uint32_t>(k));
}

// The queued big files, each resumable like a single file's transfer, as many at a time as the
// folder may move ("At once": Job::files_at_once, asked again as each file ends), the highest
// priority's first (next_queued). They share the folder's connections (same pool id) by their
// priorities and split ranges to keep them busy. `big_file(BigFile)` moves one (BigEnd), called
// from several threads.
template <class Job, class BigOne>
void run_big(const Job& job, Tree& tree, std::mutex& m, Members& members, const std::stop_token& stop, BigOne& big_file) {
    constexpr int most = 8; // "At once" goes up to 8
    auto worker = [&] {
        for (;;) {
            if (stop.stop_requested()) return;
            const int may = allowed(job); // without the lock: the window's answer takes its own
            std::size_t i = 0;
            std::stop_source file_stop;
            bool wait = false;
            {
                std::lock_guard lock(m);
                if (!next_queued(tree, true)) return;
                wait = tree.files_at_once >= may;
            }
            if (wait) { // as many as it may are running: another starts when one ends or more are allowed
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            {
                std::lock_guard lock(m);
                const auto next = next_queued(tree, true);
                if (!next) return;
                if (tree.files_at_once >= may) continue; // another worker took the place
                ++tree.files_at_once;
                i = *next;
                members.big.push_back(Members::Big{.index = i});
                auto& f = tree.files[i];
                f.status = State::running;
                f.waiting = true; // until a stream reads
                f.error.clear();
                f.stop = file_stop;
                f.stop_reason = StopReason::none;
            }
            notify(job, Event::changed);
            std::stop_callback forward(stop, [&file_stop] { file_stop.request_stop(); });
            BigFile b;
            b.index = i;
            b.member = big_member(i);
            // Its part of the folder's connections and of its share of a speed limit, by its
            // priority among the folder's running files: one that starts later gets its part
            // instead of waiting for the first ones to end, and a priority changed meanwhile
            // counts at the next reads. Of the connections the small files' workers don't claim
            // (Tree::small_claim): none while they claim the whole share, more as they end.
            b.streams_cap = [&, i] {
                const int share = job.connections->share(job.server, job.id); // without the lock
                std::lock_guard lock(m);
                const auto at = std::ranges::find(members.big, i, &Members::Big::index) - members.big.begin();
                const auto parts = connection_parts(share - tree.small_claim, members.big_weights(tree));
                return at < std::ssize(parts) ? parts[static_cast<std::size_t>(at)] : 1;
            };
            // Only a file that reads takes the folder's turns, and one that doesn't is
            // shown waiting: before its first read, and when its streams lost their connections,
            // but not once all its bytes are in (its last stream ending before it is put in place
            // flashed it Queued just before Done).
            b.on_reading = [&, i](int delta, bool all_in) {
                std::lock_guard lock(m);
                if (auto* big = members.find(i)) {
                    big->reading += delta;
                    big->all_in = big->all_in || all_in;
                    tree.files[i].waiting = big->reading <= 0 && !big->all_in;
                }
            };
            b.flow_part = [&, i] {
                std::lock_guard lock(m);
                return static_cast<double>(tree.files[i].weight);
            };
            b.on_progress = [&, i](const fetch::Progress& p) {
                {
                    std::lock_guard lock(m);
                    auto& f = tree.files[i];
                    f.bytes = p.bytes;
                    f.speed = p.speed;
                    f.eta = p.eta;
                    if (p.total > 0 && p.bytes >= p.total) { // there already, or put in place: no wait
                        f.waiting = false;
                        if (auto* big = members.find(i)) big->all_in = true;
                    }
                    tree.connections = p.connections;
                }
                notify(job, Event::progress);
            };
            b.stop = file_stop.get_token();
            const BigEnd end = big_file(b);

            bool discard = false;
            {
                std::lock_guard lock(m);
                --tree.files_at_once;
                std::erase_if(members.big, [i](const Members::Big& big) { return big.index == i; });
                auto& f = tree.files[i];
                f.waiting = false;
                f.speed = 0;
                f.eta.reset();
                switch (end.kind) {
                case BigEnd::Kind::done:
                    f.status = State::done;
                    f.fetched = f.fetched || end.moved;
                    // The file as moved, which verification proves against.
                    f.size = end.size;
                    f.modtime = end.modtime;
                    f.bytes = f.size;
                    break;
                case BigEnd::Kind::failed:
                    f.status = State::failed;
                    f.error = end.error;
                    break;
                case BigEnd::Kind::skipped: // the choice kept what was there
                    f.status = State::skipped;
                    f.error = end.error;
                    break;
                case BigEnd::Kind::stopped:
                    f.status = f.stop_reason == StopReason::pause  ? State::paused
                               : f.stop_reason == StopReason::skip ? State::skipped
                                                                   : State::queued; // the folder stopped
                    if (f.status == State::queued) ++tree.requeued;
                    discard = f.status == State::skipped;
                    break;
                }
                if (end.under_lock) end.under_lock();
            }
            if (discard && end.discard) end.discard();
            notify(job, Event::changed);
        }
    };
    std::vector<std::jthread> workers;
    for (int k = 0; k < most; ++k) workers.emplace_back(worker);
}

// Moves the queued files until none is left or `stop`: the small files on every free connection
// beside the big ones under At once, a pass at a time until none is queued (a paused one resumed,
// a failed one retried). `big_pass()` gives a pass's big_file (run_big) before its big files
// start. Files still running at a stop are queued again, ready for the next run. Without a pool
// (never set up) nothing moves.
template <class Job, class SmallOn, class BigPass>
void run(const Job& job, Tree& tree, std::mutex& m, const std::stop_token& stop, SmallOn& small_on, BigPass& big_pass) {
    {
        std::lock_guard lock(m);
        set_big(tree.files);
    }
    job.connections->join(job.server, job.id, job.weight != nullptr ? job.weight->load() : 2);
    while (job.pool && !stop.stop_requested()) {
        bool small_left = false;
        bool big_left = false;
        {
            std::lock_guard lock(m);
            for (const auto& f : tree.files) {
                if (f.status != State::queued) continue;
                (f.big ? big_left : small_left) = true;
            }
            // Each pass looks from the first file again: a file queued again without saying so
            // waits for the next pass instead of being missed for good.
            ++tree.requeued;
        }
        if (!small_left && !big_left) break;
        Members members; // the files running in this pass
        {
            std::jthread small_files;
            if (small_left) {
                // The small files' claim holds before the big files start, however late their
                // thread runs.
                const int workers = std::max(1, job.connections->budget(job.server));
                {
                    std::lock_guard lock(m);
                    tree.small_claim = workers;
                }
                small_files = std::jthread([&, workers] { run_small(job, tree, m, workers, stop, small_on); });
            }
            if (big_left) {
                auto big_file = big_pass();
                run_big(job, tree, m, members, stop, big_file);
            }
        }
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
        ++tree.requeued;
    }
    notify(job, Event::changed);
}

} // namespace grab::folder::sched
