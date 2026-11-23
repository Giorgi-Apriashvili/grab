#include "folder_job.hpp"

#include "existing.hpp"
#include "folder_sched.hpp"
#include "local_name.hpp"
#include "rclone_encoding.hpp"
#include "remote.hpp"
#include "sftp.hpp"
#include "ssh_walk.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace grab::folder {

namespace fs = std::filesystem;

namespace {

using sched::notify;
using sched::small_weight;
using sched::SmallRun;

// Small files on one connection (run_small_grab): up to 32 in flight, each OPEN, then its READs
// and an FSTAT (the size and time now, which may have changed since the listing), then CLOSE, all
// pipelined; 8 MiB of READs in flight at most. Under a speed limit (asked at each pass, as the
// schedule may change it) as many files, but READs of an eighth of a second of the limit's worth
// and at most a quarter second's in flight, each taking its tokens before it goes, as the folder's
// member `member` (Members): the limiter bounds the rate, while the files and READs on their way
// keep the connection busy over its round trip (one file and one READ at a time reached half of a
// 1 MiB/s limit on Main, chunk F's L4). The next READ keeps its place in the limiter's line while
// the replies on their way are read (RateLimiter::enqueue, ready after each, as fetch's streams),
// so none waits unread through token turns (with many workers sharing a limit, seconds). The limit
// is looked at after each reply: switched on or lowered while more is on its way than it now
// allows (8 MiB at full speed, which kept coming at full speed), the turn ends at once as a broken
// connection's does (`cut`: its connection dropped, a new login), as fetch's streams do; less on
// its way is charged to the new rate. Files left half-way by a stop or a broken connection go back
// to the queue; by a cut, they stay running, `carried` to the worker's next turn, which takes them
// first (they don't flash Queued while it logs in again). The next file is the highest priority's
// (next_queued).
SmallRun small_on(const Job& job, Tree& tree, std::mutex& m, std::uint32_t member, sftp::Client& c,
                  const fs::path& dir, const std::stop_token& stop, std::vector<std::size_t>& carried) {
    constexpr std::size_t max_files = 32;
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
    const auto flow = rate::flow_of(job.id, member);
    // The limit as this turn last saw it, looked at again at each pass (after each reply), and the
    // next READ's place in the limiter's line while its tokens come; given up on every way out.
    rate::RateLimiter::Setting seen = job.limiter != nullptr ? job.limiter->setting() : rate::RateLimiter::Setting{};
    rate::Place place(job.limiter);
    // The folder's weight in the limiter now: its priority may have changed.
    auto folder_weight = [&] { return job.weight != nullptr ? static_cast<double>(job.weight->load()) : 2.0; };

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
            if (job.on_received) job.on_received(r->size());
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
                // Conflicts were answered before; one that appeared since stays.
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
                // The server's file as fetched (FSTAT), which verification proves against.
                f.size = s.size;
                f.modtime = s.modtime;
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
                    std::optional<std::size_t> next;
                    while (!next && !carried.empty()) { // those a cut left first, unless paused or skipped since
                        const std::size_t i = carried.front();
                        carried.erase(carried.begin());
                        if (tree.files[i].status == State::running) next = i;
                    }
                    if (!next) next = next_queued(tree, false);
                    if (!next) {
                        taking = false;
                        break;
                    }
                    auto& f = tree.files[*next];
                    f.status = State::running;
                    f.bytes = 0;
                    f.error.clear();
                    s.index = *next;
                    s.size = f.size;
                    s.modtime = f.modtime;
                    s.target = target_of(dir, f);
                    path = f.path;
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
            // A limit switched on or lowered since the last look (above).
            if (job.limiter != nullptr) {
                if (const auto now = job.limiter->setting(); now.generation != seen.generation) {
                    place.drop(); // asked for at the old rate
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
            // READs for what the open files still lack; while Play on this server warms up or waits
            // for bytes, no more than its depth's worth (fetch::Connections::urgent). Under a speed
            // limit, smaller and fewer, as above.
            const std::uint64_t rate = seen.rate;
            const std::uint64_t read_size = rate > 0 ? fetch::limited_read(rate, 1) : sftp::max_read;
            std::uint64_t most = std::min(
                max_bytes, std::uint64_t{sftp::max_read} *
                               static_cast<std::uint64_t>(job.connections->read_depth(job.server, fetch::grab_depth)));
            if (rate > 0) most = std::min(most, fetch::limited_in_flight(rate, 1));
            bool held = false;      // its next READ waits for tokens while replies are read
            bool new_rate = false;  // waiting for them ended with a new rate: looked at first
            for (auto it = slots.begin(); it != slots.end() && !held && !new_rate; ++it) {
                auto& [id, s] = *it;
                while (s.opened && !s.error && !s.done && s.asked < s.size && !stop.stop_requested()) {
                    const auto n = static_cast<std::uint32_t>(std::min(read_size, s.size - s.asked));
                    if (reads_in_flight > 0 && bytes_in_flight + n > most) break;
                    if (rate > 0) {
                        // Its tokens are there now, or (with nothing on its way to read) it waits for
                        // them; else replies are read meanwhile and it looks again after each.
                        if (place.ticket == 0) place.ticket = job.limiter->enqueue(n, flow, folder_weight(), small_weight);
                        if (place.ticket != 0 && !job.limiter->ready(place.ticket)) {
                            if (c.pending() > 0) {
                                held = true;
                                break;
                            }
                            if (!job.limiter->wait(place.ticket, stop)) { // a new rate, or a stop: given up
                                place.ticket = 0;
                                new_rate = !stop.stop_requested();
                                break;
                            }
                        }
                        place.ticket = 0;
                    }
                    send_read(id, s.asked, n);
                    s.asked += n;
                    // Under a limit on its way at once, right after its tokens, not with the next
                    // pump: the network follows the limit evenly.
                    if (rate > 0) c.flush();
                }
            }
            if (stop.stop_requested()) break;
            if (new_rate) continue;
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

// One big file (sched::run_big's), resumable like a single-file download (fetch::run), into its
// place in `dir`; `reserved`: the names of the folder's files (reserved_names).
sched::BigEnd fetch_big(const Job& job, Tree& tree, std::mutex& m, const fs::path& dir, const fetch::Reserved& reserved,
                        const sched::BigFile& b) {
    std::string path;
    fs::path target;
    {
        std::lock_guard lock(m);
        const auto& f = tree.files[b.index];
        path = f.path;
        target = target_of(dir, f);
    }
    fetch::Download d;
    d.source = fetch::Source{.rclone_remote = job.ctx->settings.rclone_remote, .path = child_remote_path(job.remote_path, path)};
    d.server = job.server;
    d.id = job.id;
    d.target = target;
    d.reserved = reserved;
    d.replace = job.existing == existing::Choice::replace;
    d.joined = true;
    d.limiter = job.limiter;
    d.weight = job.weight;
    d.streams_cap = b.streams_cap;
    d.on_reading = b.on_reading;
    d.on_received = job.on_received;
    d.member = b.member;
    d.flow_part = b.flow_part;
    d.on_command = job.on_command;
    d.pool = job.pool;
    std::error_code ec;
    fs::create_directories(d.target.parent_path(), ec);
    const auto result = fetch::run(d, *job.connections, b.on_progress, b.stop);

    sched::BigEnd end;
    switch (result.outcome) {
    case fetch::Outcome::done:
        end.kind = sched::BigEnd::Kind::done;
        end.moved = !result.present;
        // The server's file as fetched, which verification proves against.
        end.size = result.size;
        end.modtime = result.modtime;
        break;
    case fetch::Outcome::failed:
    case fetch::Outcome::exists: // appeared since the question: Retry asks
        end.kind = sched::BigEnd::Kind::failed;
        end.error = result.error;
        break;
    case fetch::Outcome::stopped:
        end.kind = sched::BigEnd::Kind::stopped;
        end.discard = [at = d.target, from = d.source.path, names = d.reserved, rclone_remote = d.source.rclone_remote] {
            fetch::discard(at, from, names, rclone_remote);
        };
        break;
    }
    return end;
}

} // namespace

std::vector<int> connection_parts(int free, std::span<const int> weights) {
    const auto n = weights.size();
    if (free <= 0) return std::vector<int>(n, 0); // the small files claim them all: the big files wait
    std::vector<int> parts(n, 1);
    std::int64_t all = 0;
    for (const int w : weights) all += std::max(0, w);
    if (all <= 0) return parts; // the server's budget decides
    if (std::cmp_less(free, n)) {
        // Fewer than the files: one each for the highest priorities (the earlier file first); the
        // others wait, rather than a Low file reading while a High one waits.
        std::vector<std::size_t> order(n);
        for (std::size_t k = 0; k < n; ++k) order[k] = k;
        std::ranges::stable_sort(order, [&](std::size_t x, std::size_t y) { return weights[x] > weights[y]; });
        for (std::size_t k = 0; k < n; ++k) parts[order[k]] = std::cmp_less(k, free) ? 1 : 0;
        return parts;
    }
    // free × w / Σw rounded down, then the connections left over one each by the largest remainder.
    std::vector<std::int64_t> rest(n);
    int given = 0;
    for (std::size_t k = 0; k < n; ++k) {
        const std::int64_t share = std::int64_t{free} * std::max(0, weights[k]);
        parts[k] = static_cast<int>(share / all);
        rest[k] = share % all;
        given += parts[k];
    }
    std::vector<std::size_t> order(n);
    for (std::size_t k = 0; k < n; ++k) order[k] = k;
    std::ranges::stable_sort(order, [&](std::size_t x, std::size_t y) {
        return rest[x] != rest[y] ? rest[x] > rest[y] : weights[x] > weights[y];
    });
    for (std::size_t k = 0; given < free; k = (k + 1) % n, ++given) ++parts[order[k]];
    // Every file at least 1, taken from the biggest part (there is one over 1: free ≥ n).
    for (auto& p : parts) {
        if (p > 0) continue;
        ++p;
        --*std::ranges::max_element(parts);
    }
    return parts;
}

File make_file(std::string path, std::uint64_t size, std::string modtime) {
    File f;
    f.path = std::move(path);
    f.size = size;
    f.modtime = std::move(modtime);
    f.big = size >= small_limit;
    return f;
}

void settle_names(std::vector<File>& files) {
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
        if (taken.insert(local::key(locals[i])).second) continue; // the first with this name
        // An earlier file, or a folder, has this name on Windows: the next free number.
        const std::string name = util::path_to_utf8(locals[i].filename());
        bool given = false;
        for (int n = 2; n < 1000 && !given; ++n) {
            auto numbered = locals[i].parent_path() / util::path_from_utf8(existing::numbered(name, n));
            const auto k = local::key(numbered);
            if (names.contains(k) || !taken.insert(k).second) continue;
            f.local = std::move(numbered);
            given = true;
        }
        if (!given) {
            f.status = State::failed;
            f.error = "no free name for it: other files of the folder have its name on Windows";
        }
    }
}

void set_big(std::vector<File>& files) {
    for (auto& f : files) f.big = f.size >= small_limit;
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

bool pause_file(Tree& tree, std::size_t index) {
    if (index >= tree.files.size()) return false;
    auto& f = tree.files[index];
    if (f.status == State::queued) {
        f.status = State::paused;
    } else if (f.status == State::running && f.big) {
        if (f.stop_reason != StopReason::none) return false;
        f.stop_reason = StopReason::pause; // run_big marks it paused once stopped
        f.stop.request_stop();
    } else if (f.status == State::running) {
        f.status = State::paused; // a small file: its worker drops it once its replies are in
        f.speed = 0;
        f.bytes = 0; // a small file is kept in memory until it's whole: it starts over on resume
    } else {
        return false;
    }
    return true;
}

bool resume_file(Tree& tree, std::size_t index) {
    if (index >= tree.files.size() || tree.files[index].status != State::paused) return false;
    tree.files[index].status = State::queued;
    ++tree.requeued;
    return true;
}

bool retry_file(Tree& tree, std::size_t index) {
    if (index >= tree.files.size() || tree.files[index].status != State::failed) return false;
    auto& f = tree.files[index];
    f.status = State::queued;
    f.error.clear();
    ++tree.requeued;
    return true;
}

void resume_paused(Tree& tree) {
    for (auto& f : tree.files) {
        if (f.status == State::paused) f.status = State::queued;
    }
    ++tree.requeued;
}

std::optional<std::size_t> next_queued(Tree& tree, bool big) {
    if (tree.cursors_at != tree.requeued) {
        tree.cursors = {};
        tree.cursors_at = tree.requeued;
    }
    const auto& files = tree.files;
    const std::size_t n = files.size();
    auto& cursors = tree.cursors[big ? 1 : 0];
    constexpr std::array<int, 3> weights{4, 2, 1}; // high first
    for (std::size_t w = 0; w < weights.size(); ++w) {
        auto& at = cursors[w];
        while (at < n && !(files[at].status == State::queued && files[at].big == big &&
                           priority_weight(files[at].weight) == weights[w])) {
            ++at;
        }
        if (at < n) return at; // not passed: the caller starts it, and the next look steps over it
    }
    return std::nullopt;
}

bool set_priority(Tree& tree, std::size_t index, int weight) {
    if (index >= tree.files.size()) return false;
    auto& f = tree.files[index];
    weight = priority_weight(weight);
    if (f.status == State::done || f.status == State::skipped || f.weight == weight) return false;
    f.weight = weight;
    ++tree.requeued; // its place in the start order moved
    return true;
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

std::optional<Discard> skip_file(Tree& tree, const fs::path& dir, const std::string& remote_path, std::size_t index,
                                 bool* skipped) {
    if (skipped != nullptr) *skipped = false;
    if (index >= tree.files.size()) return std::nullopt;
    auto& f = tree.files[index];
    std::optional<Discard> discard;
    if (f.status == State::queued || f.status == State::paused || f.status == State::failed) {
        f.status = State::skipped;
        f.error.clear(); // skipped by hand: a failure's reason no longer says why (queue_view shows it)
        if (f.big) discard = Discard{target_of(dir, f), child_remote_path(remote_path, f.path), {}};
    } else if (f.status == State::running && f.big) {
        if (f.stop_reason == StopReason::skip) return std::nullopt;
        f.stop_reason = StopReason::skip; // run_big discards its partial file
        f.stop.request_stop();
    } else if (f.status == State::running) {
        f.status = State::skipped; // a small file: its worker drops it once its replies are in
        f.speed = 0;
    } else {
        return std::nullopt;
    }
    if (skipped != nullptr) *skipped = true;
    return discard;
}

void retry_files(Tree& tree, bool all_unfinished) {
    for (auto& f : tree.files) {
        if (f.status == State::failed || (all_unfinished && f.status != State::done && f.status != State::skipped)) {
            f.status = State::queued;
            f.error.clear();
        }
    }
    ++tree.requeued;
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

std::string failed_text(const Outcome& o, std::string_view checked) {
    std::string text = std::format("{} file{} failed. {}", o.failed, o.failed == 1 ? "" : "s", o.first_error);
    if (!checked.empty()) text += std::format(" · {}", checked);
    return text;
}

std::string unreadable_text(const std::vector<std::string>& unreadable) {
    if (unreadable.empty()) return {};
    const std::size_t n = unreadable.size();
    return std::format("{} couldn't be read: {}{}", n == 1 ? "1 folder" : std::format("{} folders", n),
                       unreadable.front(), n > 1 ? ", ..." : "");
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
        ++tree.requeued; // other files
    }
    notify(job, Event::checkpoint);
}

} // namespace

std::expected<void, std::string> list(const Job& job, Tree& tree, std::mutex& m, std::stop_token stop) {
    {
        std::lock_guard lock(m);
        if (tree.listed) return {};
    }
    if (!job.pool) return util::fail("no connection to the server was set up for this folder");
    // An SFTP walk on the folder's pooled connections, as many as a listing may take from the
    // server's budget (fetch::Listing, review G191).
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
        set_big(tree.files); // which ones may have a partial download
        ++tree.requeued;     // a file's kind may have changed
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
    }
}

void run(const Job& job, Tree& tree, std::mutex& m, std::stop_token stop) {
    // The folder's scheduling (folder_sched.hpp) with a download's small files (small_on) and big
    // ones (fetch_big), each pass's big files kept off the names of the folder's own files.
    const auto dir = job_dir(job);
    auto small_turn = [&](std::uint32_t member, sftp::Client& c, const std::stop_token& st, std::vector<std::size_t>& carried) {
        return small_on(job, tree, m, member, c, dir, st, carried);
    };
    auto big_pass = [&] {
        fetch::Reserved reserved;
        {
            std::lock_guard lock(m);
            reserved = reserved_names(dir, tree);
        }
        return [&, reserved = std::move(reserved)](const sched::BigFile& b) { return fetch_big(job, tree, m, dir, reserved, b); };
    };
    sched::run(job, tree, m, stop, small_turn, big_pass);
}

std::string Checked::summary() const { return verify::summary(report, true); }

Checked verify_files(const Job& job, Tree& tree, std::mutex& m, verify::Request base, std::stop_token stop) {
    Checked c;
    verify::Request& r = base;
    const fs::path dir = job_dir(job);
    std::vector<std::size_t> which; // r.files[k] is tree.files[which[k]]
    {
        std::lock_guard lock(m);
        for (std::size_t i = 0; i < tree.files.size(); ++i) {
            const auto& f = tree.files[i];
            if (f.status != State::done || !f.fetched || f.verified) continue;
            // By its own path, the name not UTF-8 too (review G17): grab hashes the raw bytes.
            r.files.push_back(verify::File{.path = f.path, .local = target_of(dir, f), .size = f.size, .modtime = f.modtime});
            which.push_back(i);
        }
    }
    if (r.files.empty()) return c;
    c.files = r.files.size();
    r.ctx = job.ctx;
    r.remote_dir = job.remote_path;
    r.connections = job.connections;
    r.server = job.server;
    r.id = job.id;
    r.joined = false;
    r.many = true;
    if (job.on_command) r.on_command = job.on_command;
    // Each file's state as its result comes, so a stop keeps the files checked so far.
    r.on_file = [&](std::size_t k, const verify::Result& result, std::size_t) {
        std::lock_guard lock(m);
        auto& f = tree.files[which[k]];
        switch (result.verdict) {
        case verify::Verdict::same: f.verified = true; break;
        case verify::Verdict::differs: {
            std::error_code ec;
            fs::remove(target_of(dir, f), ec); // not the server's file
            f.status = State::failed;
            f.error = "differs from the server's copy; deleted";
            f.fetched = false;
            f.bytes = 0;
            break;
        }
        case verify::Verdict::missing:
            f.status = State::failed;
            f.error = "the downloaded copy is gone";
            f.fetched = false;
            f.bytes = 0;
            break;
        case verify::Verdict::unchecked: break; // kept, done; checked again by the next run
        }
    };
    c.report = verify::run(r, std::move(stop));
    notify(job, Event::changed);
    return c;
}

} // namespace grab::folder
