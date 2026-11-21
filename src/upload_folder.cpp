#include "upload_folder.hpp"

#include "existing.hpp"
#include "folder_sched.hpp"
#include "local_walk.hpp"
#include "rclone_encoding.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"
#include "ssh_walk.hpp"
#include "transfer.hpp"
#include "upload.hpp"
#include "util.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace grab::upload {

namespace fs = std::filesystem;
using folder::Event;
using folder::State;
namespace sched = folder::sched;

namespace {

void notify(const FolderJob& job, Event e) {
    if (job.on_event) job.on_event(e);
}

// The folder as it is shown: grab's form, or "the login folder".
std::string shown_dir(std::string_view dir) { return dir.empty() ? std::string("the login folder") : std::string(dir); }

std::int64_t seconds_of(std::int64_t ns) {
    return ns >= 0 ? ns / 1'000'000'000 : -((-ns + 999'999'999) / 1'000'000'000);
}

// "2023-05-06T11:18:12.123456789Z", as local_walk gives a file's time.
std::string rfc3339_ns(std::int64_t ns) {
    return std::format("{:%FT%TZ}", std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{ns}});
}

// A time in seconds as SFTP sets it.
std::uint32_t mtime32(std::int64_t seconds) {
    return static_cast<std::uint32_t>(std::clamp<std::int64_t>(seconds, 0, std::numeric_limits<std::uint32_t>::max()));
}

// A file's time from the walk, in seconds (0 when it can't be read).
std::int64_t file_seconds(const folder::File& f) { return seconds_of(util::parse_rfc3339(f.modtime).value_or(0)); }

// Its Keep-both name's path below the folder (File::local holds it, '/'-separated).
std::string keep_both_rel(const folder::File& f) {
    std::string s = util::path_to_utf8(f.local);
    std::ranges::replace(s, '\\', '/');
    return s;
}

// A path below the folder ("" the folder itself) as the server's path, grab's form.
std::string below(const FolderJob& job, std::string_view rel) {
    return rel.empty() ? job.remote_path : folder::child_remote_path(job.remote_path, std::string(rel));
}

// The folder a path below the folder is in ("" the folder itself), and how deep it is.
std::string parent_of(std::string_view rel) {
    const auto slash = rel.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(rel.substr(0, slash));
}
int depth_of(std::string_view rel) { return rel.empty() ? 0 : static_cast<int>(std::ranges::count(rel, '/')) + 1; }
// Whether `rel` is `folder` or lies below it ("" holds everything).
bool inside(std::string_view rel, std::string_view folder) {
    return folder.empty() || rel == folder || (rel.starts_with(folder) && rel.size() > folder.size() && rel[folder.size()] == '/');
}

// The file's own existing-file choice: Replace for grab's own copy verify found different, what
// the user said for it, Keep both when it has a numbered name already, else the upload's.
Existing choice_of(const FolderJob& job, const Tree& tree, std::size_t i) {
    if (tree.resend.contains(i)) return Existing::replace;
    if (const auto it = tree.choices.find(i); it != tree.choices.end()) return it->second;
    return tree.files[i].local.empty() ? job.existing : Existing::keep_both;
}

std::string already_there(const FolderJob& job) { return std::format("a file of that name is already on {}", job.server); }

// The refusals of a run that hold for more than one file (Refusal::scope): the whole server's
// (read-only, full), or a folder's (no write access, no such folder), by the folder (grab's form).
// Under the tree's mutex.
struct Refused {
    std::optional<std::string> server;
    std::map<std::string, std::string> folders;
};

// A refusal for the whole server (`dir` nullopt) or the folder `dir`: noted, and the queued files
// it holds for fail with it now (big ones too), rather than each being sent to be refused.
void refuse_queued(const FolderJob& job, Tree& tree, std::mutex& m, Refused& refused, const std::optional<std::string>& dir,
                   const std::string& why) {
    {
        std::lock_guard lock(m);
        if (!dir) {
            if (refused.server) return;
            refused.server = why;
        } else if (!refused.folders.emplace(*dir, why).second) {
            return;
        }
        for (auto& f : tree.files) {
            if (f.status != State::queued) continue;
            if (dir && split_remote(target_of(job, f)).first != *dir) continue;
            f.status = State::failed;
            f.error = why;
        }
    }
    notify(job, Event::changed);
}

// What the user is told for `status`, a change refused in the folder `dir` (grab's form): what the
// run knows already for the server or that folder, else refusal()'s answer (statvfs asked), which,
// when it holds for more than the file, fails the queued files it holds for (refuse_queued).
std::string refused_in(const FolderJob& job, Tree& tree, std::mutex& m, Refused& refused, sftp::Client& c, const Caps& caps,
                       const std::string& dir, const sftp::Status& status, std::uint64_t needed) {
    {
        std::lock_guard lock(m);
        if (refused.server) return *refused.server;
        if (const auto it = refused.folders.find(dir); it != refused.folders.end()) return it->second;
    }
    const Refusal r = refusal(c, caps, job.server, dir, rclone_enc::raw_path(dir), status, needed);
    switch (r.scope) {
    case Refusal::Scope::server: refuse_queued(job, tree, m, refused, std::nullopt, r.why); break;
    case Refusal::Scope::folder: refuse_queued(job, tree, m, refused, dir, r.why); break;
    case Refusal::Scope::file: break;
    }
    return r.why;
}

// Runs `step(client)` on a connection of the pool, a broken reused one tried once more. Without a
// seat of the budget's: the caller holds one.
template <class T, class Step>
std::expected<T, std::string> on_lease(const FolderJob& job, const std::stop_token& stop, Step&& step) {
    for (bool retried = false;;) {
        std::optional<ssh::Pool::Lease> lease;
        try {
            lease = job.pool->acquire(stop, ssh::interactive_retry);
            if (!lease) return util::fail("stopped");
            return step((*lease)->sftp());
        } catch (const ssh::Error& e) {
            return util::fail(e.what());
        } catch (const sftp::Unavailable& e) {
            if (lease) lease->broken();
            return util::fail(e.what());
        } catch (const sftp::Broken& e) {
            if (lease) lease->broken();
            if (lease && lease->reused() && !std::exchange(retried, true) && !stop.stop_requested()) continue;
            return util::fail(e.what());
        }
    }
}

// Removes the temps the upload left on the server (Tree::temps), all at once: those that went, or
// were gone already, leave the list.
void remove_temps(const FolderJob& job, Tree& tree, std::mutex& m, sftp::Client& c) {
    std::vector<std::string> temps;
    {
        std::lock_guard lock(m);
        temps.assign(tree.temps.begin(), tree.temps.end());
    }
    if (temps.empty()) return;
    std::vector<std::string> gone;
    for (const auto& t : temps) {
        if (!is_temp_name(rclone_enc::raw_path(split_remote(t).second))) continue; // never anything not grab's
        if (job.on_command) job.on_command({"sftp", "rm", t});
        c.remove(rclone_enc::raw_path(t), [&gone, t](std::expected<void, sftp::Status> r) {
            if (r || r.error().code == sftp::Code::no_such_file) gone.push_back(t);
        });
    }
    while (c.pending() > 0) c.pump();
    std::lock_guard lock(m);
    for (const auto& t : gone) tree.temps.erase(t);
}

// The server folders the upload needs, top-down, a level at a time, each level's MKDIRs together;
// none for those look() saw there (the folder itself when merging). One that is refused is looked
// at (OpenSSH answers "Failure" for a name taken; other servers say "permission denied" or "file
// already exists" for a folder there): a folder is merged into, a file fails that subtree. A
// folder that can't be made fails the files below it with the reason. Those it made go to
// Tree::made. False when the folder itself couldn't be made: every file failed.
bool make_folders(const FolderJob& job, Tree& tree, std::mutex& m, sftp::Client& c, const Caps& caps, Refused& refused) {
    std::vector<std::string> wanted{std::string()};
    std::unordered_set<std::string> made_before;
    {
        std::lock_guard lock(m);
        wanted.insert(wanted.end(), tree.folders.begin(), tree.folders.end());
        made_before.insert(tree.made.begin(), tree.made.end());
        if (tree.merging) made_before.insert(std::string());
        made_before.insert(tree.server_dirs.begin(), tree.server_dirs.end());
    }
    std::ranges::stable_sort(wanted, {}, [](const std::string& rel) { return depth_of(rel); });
    std::vector<std::pair<std::string, std::string>> failed; // folder, why
    auto failed_above = [&](const std::string& rel) {
        return std::ranges::any_of(failed, [&](const auto& f) { return inside(rel, f.first); });
    };
    struct Asked {
        std::string rel;
        std::optional<sftp::Status> status; // the MKDIR's failure
        std::optional<sftp::Attrs> there;   // ...and what STAT then found
    };
    constexpr std::size_t batch = 256; // requests on their way at once
    for (std::size_t from = 0; from < wanted.size();) {
        const int level = depth_of(wanted[from]);
        std::size_t to = from;
        while (to < wanted.size() && depth_of(wanted[to]) == level) ++to;
        std::vector<Asked> asked;
        for (std::size_t k = from; k < to; ++k) {
            if (made_before.contains(wanted[k]) || failed_above(wanted[k])) continue;
            asked.push_back(Asked{.rel = wanted[k], .status = {}, .there = {}});
        }
        for (std::size_t at = 0; at < asked.size(); at += batch) {
            const std::span<Asked> some(asked.data() + at, std::min(batch, asked.size() - at));
            for (Asked& a : some) {
                const std::string path = below(job, a.rel);
                if (job.on_command) job.on_command({"sftp", "mkdir", path});
                c.mkdir(rclone_enc::raw_path(path), sftp::Attrs{}, [&a](std::expected<void, sftp::Status> r) {
                    if (!r) a.status = r.error();
                });
            }
            while (c.pending() > 0) c.pump();
            // There already (or something is): what it is.
            for (Asked& a : some) {
                if (!a.status || a.status->code == sftp::Code::no_such_file) continue;
                c.stat(rclone_enc::raw_path(below(job, a.rel)), [&a](std::expected<sftp::Attrs, sftp::Status> r) {
                    if (r) a.there = *r; // else: the MKDIR's refusal says why
                });
            }
            while (c.pending() > 0) c.pump();
        }
        for (const Asked& a : asked) {
            const std::string path = below(job, a.rel);
            if (!a.status) {
                std::lock_guard lock(m);
                tree.made.push_back(a.rel);
                continue;
            }
            if (a.there && a.there->is_dir()) continue; // merged into
            std::string why;
            const std::string dir = a.rel.empty() ? split_remote(job.remote_path).first : below(job, parent_of(a.rel));
            if (a.there) {
                why = std::format("{}:{} is a file, so the folder can't be made there", job.server, path);
            } else if (a.status->code == sftp::Code::no_such_file) {
                why = std::format("there is no folder {} on {}", shown_dir(dir), job.server);
            } else {
                why = refused_in(job, tree, m, refused, c, caps, dir, *a.status, 0);
            }
            failed.emplace_back(a.rel, std::move(why));
        }
        from = to;
    }
    if (failed.empty()) return true;
    {
        std::lock_guard lock(m);
        for (auto& f : tree.files) {
            if (f.status != State::queued) continue;
            const std::string folder = parent_of(f.path);
            const auto it = std::ranges::find_if(failed, [&](const auto& x) { return inside(folder, x.first); });
            if (it == failed.end()) continue;
            f.status = State::failed;
            f.error = it->second;
        }
    }
    notify(job, Event::changed);
    return std::ranges::none_of(failed, [](const auto& f) { return f.first.empty(); });
}

// Before the files: on one connection, what the server offers (`caps`), the temps an earlier run
// left removed, and the folders made. False when nothing can be sent: a stop, the server out of
// reach (every queued file failed with why), or the folder itself not made.
bool prepare(const FolderJob& job, Tree& tree, std::mutex& m, const std::stop_token& stop, Caps& caps, Refused& refused) {
    const transfer::Seat seat(*job.connections, job.server, job.id, false, job.weight != nullptr ? job.weight->load() : 2);
    if (!job.connections->acquire(job.server, job.id, stop, false)) return false;
    auto ready = on_lease<bool>(job, stop, [&](sftp::Client& c) {
        caps = caps_of(c);
        remove_temps(job, tree, m, c);
        return make_folders(job, tree, m, c, caps, refused);
    });
    job.connections->release(job.server, job.id);
    if (stop.stop_requested()) return false;
    if (!ready) {
        {
            std::lock_guard lock(m);
            for (auto& f : tree.files) {
                if (f.status != State::queued) continue;
                f.status = State::failed;
                f.error = ready.error();
            }
        }
        notify(job, Event::changed);
        return false;
    }
    return *ready;
}

// One big file (sched::run_big's) through upload::run: resumable, its ranges over its part of the
// folder's connections.
sched::BigEnd send_big(const FolderJob& job, Tree& tree, std::mutex& m, const sched::BigFile& b) {
    Upload u;
    std::string modtime;
    {
        std::lock_guard lock(m);
        const auto& f = tree.files[b.index];
        u.local = local_walk::local_path(job.source, f.path);
        u.remote_path = target_of(job, f);
        u.existing = choice_of(job, tree, b.index);
        modtime = f.modtime;
    }
    u.server = job.server;
    u.id = job.id;
    u.joined = true;
    u.limiter = job.limiter;
    u.weight = job.weight;
    u.member = b.member;
    u.flow_part = b.flow_part;
    u.streams_cap = b.streams_cap;
    u.on_reading = b.on_reading;
    u.on_sent = job.on_sent;
    u.on_command = job.on_command;
    u.pool = job.pool;
    u.records_dir = job.records_dir;
    const Result r = run(u, *job.connections, b.on_progress, b.stop);

    sched::BigEnd end;
    const std::size_t i = b.index;
    std::string placed_rel; // Keep both at the last moment: the name it took
    switch (r.outcome) {
    case Outcome::done:
        end.kind = sched::BigEnd::Kind::done;
        end.moved = true;
        // The local file as sent, which verification proves the server's copy against.
        end.size = r.size;
        end.modtime = r.mtime_ns != 0 ? rfc3339_ns(r.mtime_ns) : modtime;
        if (r.placed != u.remote_path && r.placed.starts_with(job.remote_path + "/")) {
            placed_rel = r.placed.substr(job.remote_path.size() + 1);
        }
        break;
    case Outcome::failed:
        end.kind = sched::BigEnd::Kind::failed;
        end.error = r.error;
        break;
    case Outcome::exists: // a file appeared at its name meanwhile, and the choice is to ask
        end.kind = sched::BigEnd::Kind::failed;
        end.error = already_there(job);
        break;
    case Outcome::skipped:
        end.kind = sched::BigEnd::Kind::skipped;
        end.error = r.note;
        break;
    case Outcome::stopped:
        end.kind = sched::BigEnd::Kind::stopped;
        // Skipped: its partial upload goes (the temp by its record).
        end.discard = [pool = job.pool, records = job.records_dir, server = job.server, remote = u.remote_path] {
            if (records.empty()) return;
            [[maybe_unused]] auto gone = discard(*pool, records, server, remote);
        };
        break;
    }
    end.under_lock = [&tree, i, r, placed_rel] {
        auto& f = tree.files[i];
        if (!placed_rel.empty()) f.local = util::path_from_utf8(placed_rel);
        if (r.outcome == Outcome::done) {
            tree.resend.erase(i);
            if (const auto kept = tree.kept.find(i); kept != tree.kept.end()) { // a small file's once
                tree.temps.insert(kept->second);
                tree.kept.erase(kept);
            }
        }
        if (r.outcome == Outcome::exists) {
            tree.asking.insert(i);
            if (r.clash) tree.clashes[i] = *r.clash;
        }
        if (!r.temp.empty()) tree.temps.insert(r.temp);
    };
    return end;
}

// Small files on one connection (sched::run_small's turn): up to 32 at a time, each read whole
// from the disk and sent into a temp file of its own beside its target, OPEN (created exclusively)
// first; once it is open its WRITEs go, and right behind them FSETSTAT (the local file's time),
// CLOSE and an LSTAT of the target, all without waiting; when every reply is in, a RENAME to the
// target if nothing is there (UPLOADS.md 5.3), else upload::place by the file's choice. So each file
// costs about three round trips, overlapped with the other files'; 8 MiB of WRITEs in flight at
// most (Play's depth while it is urgent). A WRITE goes out as it is queued. Under a speed limit,
// smaller WRITEs, each after its tokens, as a folder download's small files read
// (folder_job.cpp): a limit switched on or lowered below what is on its way ends the turn (`cut`),
// its files carried to the next. A file paused or skipped meanwhile has its temp removed; one left
// by a stop or a broken connection keeps it in Tree::temps, removed at the next run.
sched::SmallRun send_small(const FolderJob& job, Tree& tree, std::mutex& m, const Caps& caps, Refused& refused,
                           std::uint32_t member, sftp::Client& c, const std::stop_token& stop, std::vector<std::size_t>& carried) {
    constexpr std::size_t max_files = 32;
    constexpr std::uint64_t max_bytes = std::uint64_t{8} << 20;
    enum class Step { checking, opening, writing, closing, renaming, removing };
    struct Slot {
        std::size_t index = 0;
        std::string target;  // grab's form
        std::string dir;     // its folder, grab's form
        std::string temp;    // grab's form
        std::string data;    // the local file's bytes
        std::int64_t mtime = 0; // seconds
        Existing choice = Existing::ask;
        std::string handle;
        Step step = Step::opening;
        int opens = 0;            // OPENs sent: a temp name taken gets another token, once
        std::uint64_t sent = 0;   // bytes WRITE'd
        std::uint64_t acked = 0;  // bytes the server acknowledged
        int pending = 0;          // requests on their way
        std::optional<sftp::Status> refused; // its OPEN or a WRITE
        bool looked = false;      // LSTAT of the target answered
        std::optional<sftp::Attrs> there;
        std::optional<sftp::Status> look_failed;
        bool renamed = false;
        // The name a rename was last sent for by upload::place (Keep both: a numbered one), or
        // the checking step's: where a broken link may have left the file (Tree::renaming).
        std::string placing;
        bool dropped = false;     // paused or skipped meanwhile: its temp goes, nothing else
        std::string why;          // removing: why it failed
        Outcome outcome = Outcome::failed; // removing: how it ends
        bool done = false;       // ended: the slot goes once its replies are in
    };
    std::map<int, Slot> slots;
    int next_id = 0;
    std::uint64_t bytes_in_flight = 0;
    int writes_in_flight = 0;
    bool taking = true;
    sched::SmallRun run;
    auto last_progress = std::chrono::steady_clock::now();
    const auto flow = rate::flow_of(job.id, member);
    rate::RateLimiter::Setting seen = job.limiter != nullptr ? job.limiter->setting() : rate::RateLimiter::Setting{};
    rate::Place turn(job.limiter);
    auto folder_weight = [&] { return job.weight != nullptr ? static_cast<double>(job.weight->load()) : 2.0; };

    // Back to the queue: the slots' files not landed yet. One whose rename upload::place had sent
    // is looked for there on its next try (Tree::renaming).
    auto requeue = [&] {
        std::lock_guard lock(m);
        for (auto& [id, s] : slots) {
            if (s.done) continue;
            if (!s.placing.empty()) tree.renaming[s.index] = s.placing;
            auto& f = tree.files[s.index];
            if (f.status != State::running) continue;
            f.status = State::queued;
            f.bytes = 0;
            f.speed = 0;
            ++tree.requeued;
        }
    };
    // Cut: the slots' files not landed yet start again on the next turn, still running.
    auto carry = [&] {
        std::lock_guard lock(m);
        for (auto& [id, s] : slots) {
            if (s.done) continue;
            auto& f = tree.files[s.index];
            if (f.status != State::running) continue;
            f.bytes = 0;
            f.speed = 0;
            carried.push_back(s.index);
        }
    };
    auto progress = [&] {
        if (const auto now = std::chrono::steady_clock::now(); now - last_progress >= std::chrono::milliseconds(100)) {
            last_progress = now;
            notify(job, Event::progress);
        }
    };
    // The file ends: its state, unless it was paused or skipped meanwhile.
    auto land = [&](Slot& s, Outcome outcome, std::string why, const std::string& placed = {}) {
        s.done = true;
        s.data = std::string(); // its memory back now, not when its last reply comes
        {
            std::lock_guard lock(m);
            auto& f = tree.files[s.index];
            f.speed = 0;
            f.eta.reset();
            if (outcome == Outcome::done || outcome == Outcome::skipped) tree.temps.erase(s.temp); // placed, or removed by place
            tree.renaming.erase(s.index);
            if (outcome == Outcome::done) {
                tree.resend.erase(s.index);
                // An earlier try's new copy, kept under its temp name: this one took the name, so
                // that is a leftover temp now, removed with the others.
                if (const auto kept = tree.kept.find(s.index); kept != tree.kept.end()) {
                    tree.temps.insert(kept->second);
                    tree.kept.erase(kept);
                }
            }
            // Paused as its name was being taken: it is there all the same.
            if (f.status == State::running || (outcome == Outcome::done && f.status == State::paused)) {
                switch (outcome) {
                case Outcome::done:
                    f.status = State::done;
                    f.fetched = true;
                    f.bytes = f.size;
                    f.error.clear();
                    if (!placed.empty() && placed != s.target && placed.starts_with(job.remote_path + "/")) {
                        f.local = util::path_from_utf8(placed.substr(job.remote_path.size() + 1));
                    }
                    break;
                case Outcome::skipped:
                    f.status = State::skipped;
                    f.error = std::move(why);
                    break;
                case Outcome::exists:
                    f.status = State::failed;
                    f.error = already_there(job);
                    tree.asking.insert(s.index);
                    break;
                case Outcome::failed:
                case Outcome::stopped:
                    f.status = State::failed;
                    f.error = std::move(why);
                    break;
                }
            }
        }
        ++run.landed;
        notify(job, Event::progress); // many a second in a folder of small files
        // Another transfer that just joined this server gets its share now: no more files for this one.
        if (taking && job.connections->yield_if_over_share(job.server, job.id)) {
            run.yielded = true;
            taking = false;
        }
    };
    // OPEN of a new temp (another token each time), created exclusively; listed in Tree::temps
    // before it goes, so a link that breaks before the answer leaves nothing unknown.
    auto send_open = [&](int id) {
        Slot& s = slots.at(id);
        const auto [dir, name] = split_remote(s.target);
        s.temp = folder::child_remote_path(dir, rclone_enc::from_raw_name(temp_name(rclone_enc::raw_path(name), new_token())));
        {
            std::lock_guard lock(m);
            tree.temps.insert(s.temp);
        }
        ++s.opens;
        ++s.pending;
        if (job.on_command) job.on_command({"sftp", "create", s.temp});
        c.open(rclone_enc::raw_path(s.temp), sftp::OpenFlags::write | sftp::OpenFlags::creat | sftp::OpenFlags::excl, sftp::Attrs{},
               [&, id](std::expected<std::string, sftp::Status> h) {
                   Slot& s = slots.at(id);
                   --s.pending;
                   if (!h) {
                       s.refused = h.error();
                       return;
                   }
                   s.handle = std::move(*h);
                   s.step = Step::writing;
               });
    };
    auto send_write = [&](int id, std::uint64_t at, std::uint32_t n) {
        Slot& s = slots.at(id);
        ++s.pending;
        ++writes_in_flight;
        bytes_in_flight += n;
        s.sent = at + n;
        c.write(s.handle, at, std::string_view(s.data).substr(static_cast<std::size_t>(at), n),
                [&, id, n](std::expected<void, sftp::Status> r) {
                    Slot& s = slots.at(id);
                    --s.pending;
                    --writes_in_flight;
                    bytes_in_flight -= n;
                    if (!r) {
                        if (!s.refused) s.refused = r.error();
                        return;
                    }
                    s.acked += n;
                    if (job.on_sent) job.on_sent(n);
                    if (!run.worked) {
                        run.worked = true;
                        job.connections->working(job.server, job.id);
                    }
                    {
                        std::lock_guard lock(m);
                        auto& f = tree.files[s.index];
                        if (f.status == State::running) f.bytes = s.acked;
                    }
                    progress();
                });
    };
    // Every byte on its way (or a refusal, or the file paused): FSETSTAT, CLOSE and the target's
    // LSTAT right behind the WRITEs, none waited for.
    auto send_close = [&](int id) {
        Slot& s = slots.at(id);
        s.step = Step::closing;
        const bool whole = !s.refused && !s.dropped;
        if (whole) {
            ++s.pending;
            // Some gateways refuse it: the file is placed all the same, with the server's own time.
            c.fsetstat(s.handle, sftp::Attrs{.size = {}, .permissions = {}, .mtime = mtime32(s.mtime)},
                       [&, id](std::expected<void, sftp::Status>) { --slots.at(id).pending; });
        }
        ++s.pending;
        c.close(s.handle, [&, id](std::expected<void, sftp::Status>) { --slots.at(id).pending; });
        if (whole) {
            ++s.pending;
            c.lstat(rclone_enc::raw_path(s.target), [&, id](std::expected<sftp::Attrs, sftp::Status> a) {
                Slot& s = slots.at(id);
                --s.pending;
                s.looked = true;
                if (a) s.there = *a;
                else if (a.error().code != sftp::Code::no_such_file) s.look_failed = a.error();
            });
        }
    };
    // The temp goes (ours: made under its token); the file then ends as `outcome` says.
    auto send_remove = [&](int id, Outcome outcome, std::string why) {
        Slot& s = slots.at(id);
        s.step = Step::removing;
        s.outcome = outcome;
        s.why = std::move(why);
        ++s.pending;
        if (job.on_command) job.on_command({"sftp", "rm", s.temp});
        c.remove(rclone_enc::raw_path(s.temp), [&, id](std::expected<void, sftp::Status> r) {
            Slot& s = slots.at(id);
            --s.pending;
            if (r || r.error().code == sftp::Code::no_such_file) {
                std::lock_guard lock(m);
                tree.temps.erase(s.temp);
            }
        });
    };
    // What the server refused, as the user is told; one that holds for the folder or the whole
    // server fails the queued files there too, and this connection takes no more once the server
    // refuses everything (refused_in).
    auto refused_why = [&](const Slot& s) {
        std::string why;
        if (s.step == Step::opening && s.refused->code == sftp::Code::no_such_file) {
            why = std::format("there is no folder {} on {}", shown_dir(s.dir), job.server);
            refuse_queued(job, tree, m, refused, s.dir, why);
        } else {
            why = refused_in(job, tree, m, refused, c, caps, s.dir, *s.refused, s.data.size());
        }
        std::lock_guard lock(m);
        if (refused.server) taking = false;
        return why;
    };
    // Anything but a plain RENAME to a free name: the rules (upload::place), waited for, the other
    // files' replies taken meanwhile. The name each rename goes to is kept in the slot (placing),
    // for a link that breaks meanwhile.
    auto place_slowly = [&](int id) {
        Slot& s = slots.at(id);
        if (s.dropped) return send_remove(id, Outcome::stopped, {}); // paused or skipped: nothing placed
        {
            std::lock_guard lock(m);
            tree.renaming.erase(s.index); // a plain RENAME's answer came: it didn't go
        }
        s.placing.clear();
        auto p = place(c, caps,
                       PlaceRequest{.server = job.server,
                                    .temp = s.temp,
                                    .target = s.target,
                                    .existing = s.choice,
                                    .size = s.data.size(),
                                    .mtime = s.mtime,
                                    .on_command = job.on_command},
                       s.placing);
        s.placing.clear(); // every answer came: where it went is known
        if (!p && p.error().kept_temp) {
            // The old file went, the new one couldn't take its name: the temp is the only copy
            // now, and the error says where it is. It stays (Tree::kept), whatever else is cleaned.
            {
                std::lock_guard lock(m);
                tree.temps.erase(s.temp);
                tree.kept[s.index] = s.temp;
            }
            return land(s, Outcome::failed, p.error().why);
        }
        if (!p) return send_remove(id, Outcome::failed, p.error().why);
        switch (p->outcome) {
        case Outcome::done: return land(s, Outcome::done, {}, p->placed);
        case Outcome::skipped: return land(s, Outcome::skipped, p->note);
        case Outcome::exists: // the window asks, showing what is there
            if (p->clash) {
                std::lock_guard lock(m);
                tree.clashes[s.index] = *p->clash;
            }
            return send_remove(id, Outcome::exists, {});
        case Outcome::failed:
        case Outcome::stopped: break;
        }
        send_remove(id, Outcome::failed, "the file couldn't be put in place");
    };
    // A slot whose replies are all in: its next step.
    auto advance = [&](int id) {
        Slot& s = slots.at(id);
        switch (s.step) {
        case Step::checking:
            // An earlier try's RENAME got there: the file at its name is this one (its size, and
            // the time grab gave it).
            if (s.there && s.there->is_file() && s.there->size == s.data.size() && s.there->mtime == mtime32(s.mtime)) {
                const std::string placed = std::exchange(s.placing, {});
                return land(s, Outcome::done, {}, placed);
            }
            s.placing.clear();
            s.looked = false;
            s.there.reset();
            s.look_failed.reset();
            if (s.dropped) {
                s.done = true;
                return;
            }
            return send_open(id);
        case Step::opening:
            if (!s.refused) return;
            if (s.refused->code == sftp::Code::failure && s.opens < 2 && !s.dropped) { // the name may be taken: another token
                {
                    std::lock_guard lock(m);
                    tree.temps.erase(s.temp);
                }
                s.refused.reset();
                return send_open(id);
            }
            {
                std::lock_guard lock(m);
                tree.temps.erase(s.temp); // not made
            }
            if (s.dropped) {
                s.done = true;
                return;
            }
            return land(s, Outcome::failed, refused_why(s));
        case Step::writing: return; // its WRITEs go from the main loop
        case Step::closing:
            if (s.dropped) return send_remove(id, Outcome::stopped, {});
            if (s.refused) return send_remove(id, Outcome::failed, refused_why(s));
            if (s.looked && !s.there && !s.look_failed) {
                // A free name: RENAME, which never replaces anything on OpenSSH.
                s.step = Step::renaming;
                {
                    std::lock_guard lock(m);
                    tree.renaming[s.index] = s.target;
                }
                ++s.pending;
                if (job.on_command) job.on_command({"sftp", "rename", s.temp, s.target});
                c.rename(rclone_enc::raw_path(s.temp), rclone_enc::raw_path(s.target), [&, id](std::expected<void, sftp::Status> r) {
                    Slot& s = slots.at(id);
                    --s.pending;
                    s.renamed = r.has_value();
                });
                return;
            }
            return place_slowly(id);
        case Step::renaming:
            if (s.renamed) return land(s, Outcome::done, {}, s.target);
            {
                std::lock_guard lock(m);
                tree.renaming.erase(s.index); // its answer came: it didn't go
            }
            if (s.dropped) return send_remove(id, Outcome::stopped, {}); // paused or skipped: no choice applied for it
            return place_slowly(id); // taken meanwhile, or refused: the rules say what now
        case Step::removing:
            if (s.dropped || s.outcome == Outcome::stopped) {
                s.done = true;
                return;
            }
            return land(s, s.outcome, s.why);
        }
    };

    try {
        for (;;) {
            // New files, up to the slots this connection keeps, each read whole first.
            while (taking && slots.size() < max_files && !stop.stop_requested()) {
                Slot s;
                std::string path;
                {
                    std::lock_guard lock(m);
                    std::optional<std::size_t> next;
                    while (!next && !carried.empty()) { // those a cut left first, unless paused or skipped since
                        const std::size_t i = carried.front();
                        carried.erase(carried.begin());
                        if (tree.files[i].status == State::running) next = i;
                    }
                    if (!next) next = folder::next_queued(tree, false);
                    if (!next) {
                        taking = false;
                        break;
                    }
                    auto& f = tree.files[*next];
                    f.status = State::running;
                    f.bytes = 0;
                    f.error.clear();
                    s.index = *next;
                    s.target = target_of(job, f);
                    s.choice = choice_of(job, tree, *next);
                    path = f.path;
                }
                s.dir = split_remote(s.target).first;
                // The local file as it is now: read whole, and read again once if it changed
                // meanwhile; one grown past small_limit goes with the big files.
                std::string local_error;
                std::optional<std::uint64_t> grown;
                for (int attempt = 0; attempt < 2; ++attempt) {
                    auto file = LocalFile::open(local_walk::local_path(job.source, path));
                    if (!file) {
                        local_error = file.error();
                        break;
                    }
                    if (file->size() >= small_limit) {
                        grown = file->size();
                        break;
                    }
                    s.data.assign(static_cast<std::size_t>(file->size()), '\0');
                    const auto got = file->read_at(0, std::span(s.data.data(), s.data.size()));
                    if (!got) {
                        local_error = got.error();
                        break;
                    }
                    s.mtime = seconds_of(file->mtime_ns());
                    if (*got == s.data.size() && !file->changed()) {
                        std::lock_guard lock(m);
                        auto& f = tree.files[s.index];
                        f.size = s.data.size();
                        f.modtime = rfc3339_ns(file->mtime_ns());
                        local_error.clear();
                        break;
                    }
                    local_error = std::format("{} changed while uploading", util::path_to_utf8(file->path()));
                }
                if (grown) {
                    {
                        std::lock_guard lock(m);
                        auto& f = tree.files[s.index];
                        f.size = *grown;
                        f.big = true;
                        f.status = State::queued;
                        ++tree.requeued;
                    }
                    notify(job, Event::changed);
                    continue;
                }
                const int id = next_id++;
                Slot& slot = slots.emplace(id, std::move(s)).first->second;
                if (!local_error.empty()) {
                    land(slot, Outcome::failed, local_error);
                    continue;
                }
                std::optional<std::string> renamed_to; // an earlier try's RENAME may have got there
                {
                    std::lock_guard lock(m);
                    if (const auto it = tree.renaming.find(slot.index); it != tree.renaming.end()) renamed_to = it->second;
                }
                if (renamed_to) {
                    slot.step = Step::checking;
                    slot.placing = *renamed_to;
                    ++slot.pending;
                    c.lstat(rclone_enc::raw_path(slot.placing), [&, id](std::expected<sftp::Attrs, sftp::Status> a) {
                        Slot& s = slots.at(id);
                        --s.pending;
                        if (a) s.there = *a;
                    });
                } else {
                    send_open(id);
                }
                notify(job, Event::progress);
            }
            // Files paused or skipped meanwhile: no more WRITEs, their temps go.
            {
                std::lock_guard lock(m);
                for (auto& [id, s] : slots) {
                    if (!s.done && tree.files[s.index].status != State::running) s.dropped = true;
                }
            }
            // A limit switched on or lowered since the last look (above).
            if (job.limiter != nullptr) {
                if (const auto now = job.limiter->setting(); now.generation != seen.generation) {
                    turn.drop(); // asked for at the old rate
                    const bool tighter = now.rate > 0 && (seen.rate == 0 || now.rate < seen.rate);
                    seen = now;
                    if (tighter && bytes_in_flight > fetch::limited_keep(seen.rate, 1)) {
                        carry();
                        run.cut = true;
                        return run;
                    }
                    if (tighter && bytes_in_flight > 0) job.limiter->debit(bytes_in_flight);
                }
            }
            // WRITEs for what the open files still have to send; while Play on this server warms
            // up or waits for bytes, no more than its depth's worth (fetch::Connections::urgent).
            // Under a speed limit, smaller and fewer, as above.
            const std::uint64_t rate = seen.rate;
            const std::uint64_t write_size = rate > 0 ? std::min<std::uint64_t>(fetch::limited_read(rate, 1), caps.write) : caps.write;
            std::uint64_t most = std::min(
                max_bytes, std::uint64_t{caps.write} * static_cast<std::uint64_t>(job.connections->read_depth(job.server, write_depth)));
            if (rate > 0) most = std::min(most, fetch::limited_in_flight(rate, 1));
            bool held = false;     // its next WRITE waits for tokens while replies are read
            bool new_rate = false; // waiting for them ended with a new rate: looked at first
            for (auto it = slots.begin(); it != slots.end() && !held && !new_rate; ++it) {
                const int id = it->first;
                Slot& s = it->second;
                if (s.done || s.step != Step::writing) continue;
                while (!s.refused && !s.dropped && s.sent < s.data.size() && !stop.stop_requested()) {
                    const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(write_size, s.data.size() - s.sent));
                    if (writes_in_flight > 0 && bytes_in_flight + n > most) break;
                    if (rate > 0) {
                        // Its tokens are there now, or (with nothing on its way) it waits for them;
                        // else replies are read meanwhile and it looks again after each.
                        if (turn.ticket == 0) turn.ticket = job.limiter->enqueue(n, flow, folder_weight(), sched::small_weight);
                        if (turn.ticket != 0 && !job.limiter->ready(turn.ticket)) {
                            if (c.pending() > 0) {
                                held = true;
                                break;
                            }
                            if (!job.limiter->wait(turn.ticket, stop)) { // a new rate, or a stop: given up
                                turn.ticket = 0;
                                new_rate = !stop.stop_requested();
                                break;
                            }
                        }
                        turn.ticket = 0;
                    }
                    send_write(id, s.sent, n);
                    // On its way at once (UPLOADS.md 5.1); the replies already here taken.
                    c.flush();
                    c.pump_ready();
                }
                if (!held && !new_rate && !stop.stop_requested() && (s.refused || s.dropped || s.sent >= s.data.size())) send_close(id);
            }
            if (stop.stop_requested()) break;
            if (new_rate) continue;
            // Steps whose replies are all in: the next one; slots that ended go.
            for (auto it = slots.begin(); it != slots.end();) {
                Slot& s = it->second;
                if (!s.done && s.pending == 0) advance(it->first);
                it = s.done && s.pending == 0 ? slots.erase(it) : std::next(it);
            }
            if (slots.empty() && !taking) break;
            if (c.pending() == 0) {
                if (slots.empty()) continue; // take more
                if (std::ranges::any_of(slots, [](const auto& e) { return e.second.step == Step::writing; })) continue;
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

} // namespace

std::string target_of(const FolderJob& job, const folder::File& f) {
    return folder::child_remote_path(job.remote_path, f.local.empty() ? f.path : keep_both_rel(f));
}

std::expected<void, std::string> walk(const FolderJob& job, Tree& tree, std::mutex& m, std::stop_token stop) {
    {
        std::lock_guard lock(m);
        if (tree.listed) return {};
    }
    auto walked = local_walk::walk(job.source, stop);
    if (stop.stop_requested()) return {};
    if (!walked) return util::fail(walked.error());
    {
        std::lock_guard lock(m);
        tree.files.clear();
        tree.files.reserve(walked->files.size());
        for (auto& f : walked->files) tree.files.push_back(folder::make_file(std::move(f.path), f.size, std::move(f.modtime)));
        tree.folders = std::move(walked->folders);
        tree.links = walked->links;
        tree.partials = walked->partials;
        tree.unreadable = std::move(walked->unreadable);
        tree.looked = false;
        tree.clashes.clear();
        tree.server_dirs.clear();
        tree.choices.clear();
        tree.resend.clear();
        tree.asking.clear();
        tree.renaming.clear();
        tree.kept.clear();
        tree.listed = true;
        ++tree.requeued; // other files
    }
    notify(job, Event::checkpoint);
    return {};
}

std::expected<void, std::string> look(const FolderJob& job, Tree& tree, std::mutex& m, std::stop_token stop) {
    if (!job.pool || job.connections == nullptr) return util::fail("no connection to the server was set up for this upload");
    // The seats a listing takes from the server's budget (fetch::Listing), the first look's too.
    fetch::Listing seat(*job.connections, job.server, stop);
    if (stop.stop_requested()) return {};
    const std::string into = split_remote(job.remote_path).first;
    if (job.on_command) job.on_command({"sftp", "stat", job.remote_path});
    auto at = on_lease<std::optional<sftp::Attrs>>(job, stop, [&](sftp::Client& c) -> std::expected<std::optional<sftp::Attrs>, std::string> {
        auto a = look_at(c, rclone_enc::raw_path(job.remote_path));
        if (!a) return util::failf("cannot look at {}:{}: {}", job.server, job.remote_path, sftp::describe(a.error()));
        if (!*a) { // nothing there: the folder it goes into must be
            const auto parent = sftp::stat(c, into.empty() ? std::string(".") : rclone_enc::raw_path(into));
            if (!parent || !parent->is_dir()) return util::failf("there is no folder {} on {}", shown_dir(into), job.server);
        }
        return *a;
    });
    if (stop.stop_requested()) return {};
    if (!at) return std::unexpected(at.error());
    if (*at && !(*at)->is_dir()) return util::failf("{}:{} is a file, so the folder can't go there", job.server, job.remote_path);
    const bool merging = at->has_value();
    // Everything below it there, folders too: a folder in a file's way is a clash of its own.
    std::vector<ssh::Walked> entries;
    if (merging) {
        if (job.on_command) job.on_command({"sftp", "walk", job.remote_path});
        ssh::WalkOptions o;
        o.connections = std::max(1, seat.connections());
        o.folders = true;
        auto walked = ssh::walk(*job.pool, job.remote_path, o, stop);
        if (stop.stop_requested()) return {};
        if (!walked) return util::fail("cannot list the folder on the server: " + walked.error());
        entries = std::move(walked->entries);
    }
    std::unordered_map<std::string, const ssh::Walked*> by_path;
    by_path.reserve(entries.size());
    for (const auto& e : entries) by_path.emplace(e.path, &e);
    {
        std::lock_guard lock(m);
        tree.clashes.clear();
        tree.on_server.clear();
        tree.server_dirs.clear();
        for (const auto& e : entries) {
            tree.on_server.insert(e.path);
            if (e.dir) tree.server_dirs.insert(e.path);
        }
        for (std::size_t i = 0; i < tree.files.size(); ++i) {
            const auto& f = tree.files[i];
            const auto it = by_path.find(f.path);
            if (it == by_path.end()) continue;
            const ssh::Walked& e = *it->second;
            tree.clashes[i] = Clash{.file = std::string(split_remote(f.path).second),
                                    .local_size = f.size,
                                    .local_mtime = file_seconds(f),
                                    .remote_size = e.size,
                                    .remote_mtime = e.modified,
                                    .remote_is_dir = e.dir};
        }
        tree.merging = merging;
        tree.looked = true;
    }
    notify(job, Event::checkpoint);
    return {};
}

void apply(const FolderJob& job, Tree& tree) {
    // Names nothing may take for a Keep-both copy: what is on the server, the folder's own files
    // and folders, and the numbers given out here.
    std::unordered_set<std::string> taken(tree.on_server.begin(), tree.on_server.end());
    for (const auto& f : tree.files) taken.insert(f.path);
    taken.insert(tree.folders.begin(), tree.folders.end());
    for (const auto& [i, clash] : tree.clashes) {
        if (tree.resend.contains(i)) continue; // grab's own copy goes again, where it is (choice_of)
        auto& f = tree.files[i];
        const bool asked = tree.asking.erase(i) > 0;
        if (f.status != State::queued && !(asked && f.status == State::failed)) continue;
        f.status = State::queued;
        f.error.clear();
        f.local.clear();
        const Decision d = decide(tree.choices.contains(i) ? tree.choices.at(i) : job.existing, clash);
        switch (d.action) {
        case Action::replace: break; // sent, and put in place by the rules
        case Action::skip:
            f.status = State::skipped;
            f.error = d.note;
            break;
        case Action::numbered: {
            const std::string folder = parent_of(f.path);
            const std::string name = split_remote(f.path).second;
            bool given = false;
            for (int n = 2; n < 1000 && !given; ++n) {
                const std::string rel = folder::child_remote_path(folder, existing::numbered(name, n));
                if (!taken.insert(rel).second) continue;
                f.local = util::path_from_utf8(rel);
                given = true;
            }
            if (!given) {
                f.status = State::failed;
                f.error = "no free name for a second copy";
            }
            break;
        }
        case Action::ask:
            f.status = State::failed;
            f.error = already_there(job);
            tree.asking.insert(i);
            break;
        }
    }
    ++tree.requeued;
}

void run(const FolderJob& job, Tree& tree, std::mutex& m, std::stop_token stop) {
    if (!job.pool || job.connections == nullptr) return; // never set up: nothing is sent
    bool looked = false;
    {
        std::lock_guard lock(m);
        looked = tree.looked;
    }
    if (!looked) {
        auto seen = look(job, tree, m, stop);
        if (stop.stop_requested()) return;
        {
            std::lock_guard lock(m);
            if (seen) {
                apply(job, tree);
            } else {
                for (auto& f : tree.files) {
                    if (f.status != State::queued) continue;
                    f.status = State::failed;
                    f.error = seen.error();
                }
            }
        }
        notify(job, Event::changed);
        if (!seen) return;
    }
    Caps caps;
    Refused refused;
    if (!prepare(job, tree, m, stop, caps, refused)) return;
    auto small_turn = [&](std::uint32_t member, sftp::Client& c, const std::stop_token& st, std::vector<std::size_t>& carried) {
        return send_small(job, tree, m, caps, refused, member, c, st, carried);
    };
    auto big_pass = [&] { return [&](const sched::BigFile& b) { return send_big(job, tree, m, b); }; };
    sched::run(job, tree, m, stop, small_turn, big_pass);
    if (stop.stop_requested()) return;
    // The partial uploads of big files skipped while they waited (skip_file's, should the caller
    // not have removed them): each temp by its record.
    std::vector<std::string> skipped;
    bool left = false;
    {
        std::lock_guard lock(m);
        for (const auto& f : tree.files) {
            if (f.big && f.status == State::skipped) skipped.push_back(target_of(job, f));
        }
        left = !tree.temps.empty();
    }
    if (!job.records_dir.empty()) {
        for (const auto& t : skipped) {
            if (!saved_progress(job.records_dir, job.server, t)) continue;
            [[maybe_unused]] auto gone = upload::discard(*job.pool, job.records_dir, job.server, t, stop);
        }
    }
    // What the files left behind on the way (a broken connection's temps) goes now.
    if (left) {
        [[maybe_unused]] auto removed = on_lease<bool>(job, stop, [&](sftp::Client& c) {
            remove_temps(job, tree, m, c);
            return true;
        });
    }
}

std::optional<std::string> skip_file(const FolderJob& job, Tree& tree, std::size_t index, bool* skipped) {
    // folder::skip_file's own Discard names a local partial download: not an upload's.
    if (!folder::skip_file(tree, job.source, job.remote_path, index, skipped)) return std::nullopt;
    return target_of(job, tree.files[index]);
}

folder::Checked verify_files(const FolderJob& job, Tree& tree, std::mutex& m, verify::Request base, std::stop_token stop) {
    folder::Checked checked;
    verify::Request& r = base;
    std::vector<std::size_t> which; // r.files[k] is tree.files[which[k]]
    {
        std::lock_guard lock(m);
        for (std::size_t i = 0; i < tree.files.size(); ++i) {
            const auto& f = tree.files[i];
            if (f.status != State::done || !f.fetched || f.verified) continue;
            r.files.push_back(verify::File{.path = f.local.empty() ? f.path : keep_both_rel(f),
                                           .local = local_walk::local_path(job.source, f.path),
                                           .size = f.size,
                                           .modtime = f.modtime});
            which.push_back(i);
        }
    }
    if (r.files.empty()) return checked;
    checked.files = r.files.size();
    r.remote_dir = job.remote_path;
    r.connections = job.connections;
    r.server = job.server;
    r.id = job.id;
    r.joined = false;
    r.many = true;
    r.upload = true;
    if (job.on_command) r.on_command = job.on_command;
    // Each file's state as its result comes, so a stop keeps the files checked so far. A mismatch
    // fails the file and its retry replaces the server's copy, grab's own; the local file stays.
    r.on_file = [&](std::size_t k, const verify::Result& result, std::size_t) {
        std::lock_guard lock(m);
        const std::size_t i = which[k];
        auto& f = tree.files[i];
        switch (result.verdict) {
        case verify::Verdict::same: f.verified = true; break;
        case verify::Verdict::differs:
            f.status = State::failed;
            f.error = "the server's copy differs (MD5)";
            f.fetched = false;
            f.bytes = 0;
            tree.resend.insert(i); // grab's own copy, wherever it went; the user's choices stand
            break;
        case verify::Verdict::missing:   // not for uploads
        case verify::Verdict::unchecked: break; // kept, done; checked again by the next run
        }
    };
    checked.report = verify::run(r, std::move(stop));
    notify(job, Event::changed);
    return checked;
}

verify::Report verify_file(verify::Request base, const fs::path& local, const std::string& placed, std::uint64_t size,
                           const std::string& modtime, std::stop_token stop) {
    base.upload = true;
    base.remote_dir.clear(); // `placed` is the whole path
    return verify::file(std::move(base), verify::File{.path = placed, .local = local, .size = size, .modtime = modtime},
                        std::move(stop));
}

Leftovers discard(const FolderJob& job, Tree& tree, std::mutex& m, std::stop_token stop) {
    Leftovers left;
    std::vector<std::string> partials; // big files' targets, whose records name their temps
    std::vector<std::string> made;
    {
        std::lock_guard lock(m);
        for (const auto& f : tree.files) {
            if (f.big && f.status != State::done) partials.push_back(target_of(job, f));
        }
        made = tree.made;
    }
    if (!job.pool) {
        std::lock_guard lock(m);
        left.temps.assign(tree.temps.begin(), tree.temps.end());
        for (const auto& rel : made) left.folders.push_back(below(job, rel));
        for (const auto& [i, t] : tree.kept) left.kept.push_back(t);
        left.partials = std::move(partials);
        return left;
    }
    // The big files' partial uploads: each temp by its record (upload::discard).
    if (!job.records_dir.empty()) {
        for (const auto& t : partials) {
            if (!saved_progress(job.records_dir, job.server, t)) continue;
            if (auto gone = upload::discard(*job.pool, job.records_dir, job.server, t, stop); !gone) left.partials.push_back(t);
        }
    }
    // The small files' temps, then the folders it made, deepest first: only an empty one goes.
    auto cleaned = on_lease<bool>(job, stop, [&](sftp::Client& c) {
        remove_temps(job, tree, m, c);
        std::vector<std::string> folders = made;
        std::ranges::stable_sort(folders, std::greater<>{}, [](const std::string& rel) { return depth_of(rel); });
        std::vector<std::string> kept_back; // couldn't be asked
        for (const auto& rel : folders) {
            const std::string path = below(job, rel);
            if (job.on_command) job.on_command({"sftp", "rmdir", path});
            auto gone = sftp::rmdir(c, rclone_enc::raw_path(path));
            // Not empty ("Failure"): it holds files sent, or someone else's; it stays, as it should.
            if (!gone && gone.error().code != sftp::Code::no_such_file && gone.error().code != sftp::Code::failure) {
                kept_back.push_back(rel);
            }
        }
        std::lock_guard lock(m);
        tree.made = kept_back;
        return true;
    });
    std::lock_guard lock(m);
    for (const auto& t : tree.temps) left.temps.push_back(t);
    for (const auto& rel : cleaned ? tree.made : made) left.folders.push_back(below(job, rel));
    for (const auto& [i, t] : tree.kept) left.kept.push_back(t);
    return left;
}

} // namespace grab::upload
