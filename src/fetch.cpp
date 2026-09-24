#include "fetch.hpp"

#include "json.hpp"
#include "process.hpp"
#include "rclone.hpp"
#include "server_ops.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <format>
#include <system_error>
#include <thread>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)
#include <winioctl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace grab::fetch {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ---- pure parts ----------------------------------------------------------------------------

std::uint64_t chunk_size_for(std::uint64_t size, int streams) {
    const std::uint64_t n = static_cast<std::uint64_t>(std::max(streams, 1));
    const std::uint64_t per_stream = (size + n - 1) / n;
    return std::clamp(per_stream, min_chunk, max_chunk);
}

std::uint64_t State::bytes_done() const {
    std::uint64_t total = 0;
    for (const auto& r : ranges) total += r.done;
    return total;
}

bool State::complete() const {
    return std::ranges::all_of(ranges, [](const Range& r) { return r.done == r.length; });
}

State new_state(std::string rclone_remote, std::string path, std::uint64_t size, std::string modtime,
                std::uint64_t chunk) {
    State s;
    s.rclone_remote = std::move(rclone_remote);
    s.path = std::move(path);
    s.size = size;
    s.modtime = std::move(modtime);
    chunk = std::max<std::uint64_t>(chunk, 1);
    for (std::uint64_t start = 0; start < size; start += chunk) {
        s.ranges.push_back(Range{start, std::min(chunk, size - start), 0});
    }
    return s;
}

std::optional<std::size_t> split(State& s, std::size_t i) {
    Range& r = s.ranges[i];
    constexpr std::uint64_t align = 64 * 1024;
    const std::uint64_t half = r.remaining() / 2 / align * align;
    if (half < min_split || r.remaining() - half < min_split) return std::nullopt;
    const Range tail{r.start + r.length - half, half, 0};
    r.length -= half; // the stream on `r` stops at the new end
    s.ranges.push_back(tail);
    return s.ranges.size() - 1;
}

std::string state_to_json(const State& s) {
    using json::Value;
    Value v = Value::make_object();
    v.set("version", Value::make_number(2));
    v.set("rcloneRemote", Value::make_string(s.rclone_remote));
    v.set("path", Value::make_string(s.path));
    v.set("size", Value::make_number(static_cast<double>(s.size)));
    v.set("modtime", Value::make_string(s.modtime));
    Value ranges = Value::make_array();
    for (const auto& r : s.ranges) {
        Value t = Value::make_array();
        t.push(Value::make_number(static_cast<double>(r.start)));
        t.push(Value::make_number(static_cast<double>(r.length)));
        t.push(Value::make_number(static_cast<double>(r.done)));
        ranges.push(std::move(t));
    }
    v.set("ranges", std::move(ranges)); // [start, length, done]
    return json::stringify(v);
}

std::optional<State> state_from_json(std::string_view text) {
    auto doc = json::parse(text);
    if (!doc || doc->kind != json::Value::Kind::object) return std::nullopt;
    auto as_u64 = [](const json::Value* v) -> std::optional<std::uint64_t> {
        if (v == nullptr || v->kind != json::Value::Kind::number || v->number < 0) return std::nullopt;
        return static_cast<std::uint64_t>(v->number);
    };
    if (as_u64(doc->find("version")) != 2u) return std::nullopt;
    State s;
    auto remote = doc->string_of("rcloneRemote");
    auto path = doc->string_of("path");
    auto modtime = doc->string_of("modtime");
    auto size = as_u64(doc->find("size"));
    const json::Value* ranges = doc->find("ranges");
    if (!remote || !path || !modtime || !size || ranges == nullptr || ranges->kind != json::Value::Kind::array) {
        return std::nullopt;
    }
    s.rclone_remote = *remote;
    s.path = *path;
    s.modtime = *modtime;
    s.size = *size;
    for (const auto& t : ranges->items) {
        if (t.kind != json::Value::Kind::array || t.items.size() != 3) return std::nullopt;
        auto start = as_u64(&t.items[0]);
        auto length = as_u64(&t.items[1]);
        auto done = as_u64(&t.items[2]);
        if (!start || !length || !done || *length == 0 || *done > *length) return std::nullopt;
        s.ranges.push_back(Range{*start, *length, *done});
    }
    // The ranges must tile [0, size) exactly.
    std::vector<Range> sorted = s.ranges;
    std::ranges::sort(sorted, {}, &Range::start);
    std::uint64_t at = 0;
    for (const auto& r : sorted) {
        if (r.start != at) return std::nullopt;
        at += r.length;
    }
    if (at != s.size) return std::nullopt;
    return s;
}

std::expected<RemoteStat, std::string> parse_stat(std::string_view json_text) {
    auto doc = json::parse(json_text);
    if (!doc || doc->kind != json::Value::Kind::object) return util::fail("unexpected answer from rclone lsjson");
    RemoteStat s;
    const json::Value* size = doc->find("Size");
    if (size != nullptr && size->kind == json::Value::Kind::number && size->number > 0) {
        s.size = static_cast<std::uint64_t>(size->number);
    }
    s.modtime = doc->string_of("ModTime").value_or("");
    const json::Value* dir = doc->find("IsDir");
    s.is_dir = dir != nullptr && dir->kind == json::Value::Kind::boolean && dir->boolean;
    return s;
}

std::optional<std::int64_t> parse_rfc3339(std::string_view t) {
    auto digits = [&](std::size_t pos, std::size_t n) -> std::optional<int> {
        if (pos + n > t.size()) return std::nullopt;
        int v = 0;
        for (std::size_t i = pos; i < pos + n; ++i) {
            if (t[i] < '0' || t[i] > '9') return std::nullopt;
            v = v * 10 + (t[i] - '0');
        }
        return v;
    };
    // YYYY-MM-DDTHH:MM:SS
    if (t.size() < 20 || t[4] != '-' || t[7] != '-' || (t[10] != 'T' && t[10] != 't' && t[10] != ' ') ||
        t[13] != ':' || t[16] != ':') {
        return std::nullopt;
    }
    auto y = digits(0, 4), mo = digits(5, 2), d = digits(8, 2), h = digits(11, 2), mi = digits(14, 2),
         s = digits(17, 2);
    if (!y || !mo || !d || !h || !mi || !s || *mo < 1 || *mo > 12 || *d < 1 || *d > 31) return std::nullopt;
    std::size_t pos = 19;
    std::int64_t nanos = 0;
    if (pos < t.size() && t[pos] == '.') {
        ++pos;
        int n = 0;
        while (pos < t.size() && t[pos] >= '0' && t[pos] <= '9') {
            if (n < 9) {
                nanos = nanos * 10 + (t[pos] - '0');
                ++n;
            }
            ++pos;
        }
        for (; n < 9; ++n) nanos *= 10;
    }
    std::int64_t offset_s = 0;
    if (pos < t.size() && (t[pos] == 'Z' || t[pos] == 'z')) {
        ++pos;
    } else if (pos < t.size() && (t[pos] == '+' || t[pos] == '-')) {
        auto oh = digits(pos + 1, 2), om = digits(pos + 4, 2);
        if (!oh || !om || pos + 3 >= t.size() || t[pos + 3] != ':') return std::nullopt;
        offset_s = (*oh * 3600 + *om * 60) * (t[pos] == '-' ? -1 : 1);
        pos += 6;
    } else {
        return std::nullopt;
    }
    if (pos != t.size()) return std::nullopt;
    // Days since 1970-01-01 (civil calendar, H. Hinnant's days_from_civil).
    const int yy = *y - (*mo <= 2 ? 1 : 0);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(yy - era * 400);
    const unsigned mp = static_cast<unsigned>((*mo + 9) % 12);
    const unsigned doy = (153 * mp + 2) / 5 + static_cast<unsigned>(*d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days = static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
    const std::int64_t secs = days * 86400 + *h * 3600 + *mi * 60 + *s - offset_s;
    return secs * 1'000'000'000 + nanos;
}

namespace {

std::vector<std::string> base_argv(const Source& src, std::initializer_list<std::string> head) {
    std::vector<std::string> argv{src.rclone};
    argv.insert(argv.end(), head);
    if (src.config) argv.insert(argv.end(), {"--config", util::path_to_utf8(*src.config)});
    argv.insert(argv.end(), src.flags.begin(), src.flags.end());
    // A dead connection is noticed in a minute; grab retries the chunk from where it stopped.
    argv.insert(argv.end(), {"--contimeout", "15s", "--timeout", "60s", "--low-level-retries", "1"});
    return argv;
}

} // namespace

std::vector<std::string> stat_argv(const Source& src) {
    return base_argv(src, {"lsjson", "--stat", remote_spec(src.rclone_remote, src.path)});
}

std::vector<std::string> cat_argv(const Source& src, std::uint64_t offset, std::uint64_t count,
                                  bool low_read_ahead) {
    auto argv = base_argv(src, {"cat", remote_spec(src.rclone_remote, src.path), "--offset", std::to_string(offset),
                                "--count", std::to_string(count)});
    // Under a speed limit grab reads slowly, and rclone would otherwise keep downloading into its
    // own read-ahead (16 MiB buffer plus 64 requests in flight, ~30 MB per stream) at full speed.
    if (low_read_ahead) argv.insert(argv.end(), {"--buffer-size", "0", "--sftp-concurrency", "4"});
    return argv;
}

fs::path part_path(const fs::path& target) {
    fs::path p = target;
    p += ".grabpart";
    return p;
}

fs::path state_path(const fs::path& target) {
    fs::path p = target;
    p += ".grabpart.json";
    return p;
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> saved_progress(const fs::path& target) {
    auto text = util::read_file(state_path(target));
    if (!text) return std::nullopt;
    auto s = state_from_json(*text);
    if (!s) return std::nullopt;
    return std::pair{s->bytes_done(), s->size};
}

void discard(const fs::path& target) {
    std::error_code ec;
    fs::remove(part_path(target), ec);
    fs::remove(state_path(target), ec);
}

// ---- connections -----------------------------------------------------------------------------

budget::ServerPool& Connections::pool(const std::string& server) {
    return pools_.try_emplace(server, 8).first->second;
}

void Connections::configure(const std::string& server, int budget) {
    std::lock_guard lock(mutex_);
    pool(server).set_budget(budget);
    changed_.notify_all();
}

void Connections::join(const std::string& server, int id) {
    std::lock_guard lock(mutex_);
    pool(server).add_user(id);
    changed_.notify_all();
}

void Connections::leave(const std::string& server, int id) {
    std::lock_guard lock(mutex_);
    pool(server).remove_user(id);
    changed_.notify_all(); // shares grow for the others
}

bool Connections::acquire(const std::string& server, int id, std::stop_token stop) {
    std::unique_lock lock(mutex_);
    bool counted = false; // registered as waiting, so downloads borrowing our share give it back
    auto done_waiting = [&] {
        if (counted) pool(server).waiting(id, -1);
    };
    for (;;) {
        if (stop.stop_requested()) {
            done_waiting();
            return false;
        }
        auto& p = pool(server);
        const auto now = budget::ServerPool::Clock::now();
        if (p.may_start(id, now)) {
            done_waiting();
            p.started(id, now);
            return true;
        }
        if (!counted) {
            p.waiting(id, +1);
            counted = true;
        }
        // Wake for the stagger gap, a released connection, or a share change.
        const auto until = std::max(p.next_start(now), now + std::chrono::milliseconds(20));
        changed_.wait_until(lock, stop, until, [] { return false; });
    }
}

int Connections::reserve(const std::string& server, int id, std::stop_token stop) {
    if (!acquire(server, id, stop)) return 0;
    std::lock_guard lock(mutex_);
    auto& p = pool(server);
    int got = 1;
    const auto now = budget::ServerPool::Clock::now();
    while (p.active(id) < p.share(id) && p.total_active() < p.budget()) {
        p.started(id, now); // rclone staggers its own connections
        ++got;
    }
    return got;
}

void Connections::release(const std::string& server, int id, int count) {
    std::lock_guard lock(mutex_);
    auto& p = pool(server);
    for (int i = 0; i < count; ++i) p.finished(id);
    changed_.notify_all();
}

bool Connections::yield_if_over_share(const std::string& server, int id) {
    std::lock_guard lock(mutex_);
    auto& p = pool(server);
    if (!p.should_yield(id)) return false;
    p.finished(id);
    changed_.notify_all();
    return true;
}

void Connections::refused(const std::string& server) {
    int lowered = 0;
    {
        std::lock_guard lock(mutex_);
        auto& p = pool(server);
        if (p.refused()) lowered = p.budget();
    }
    if (lowered > 0 && on_learned_) on_learned_(server, lowered);
}

int Connections::active(const std::string& server, int id) {
    std::lock_guard lock(mutex_);
    return pool(server).active(id);
}

int Connections::budget(const std::string& server) {
    std::lock_guard lock(mutex_);
    return pool(server).budget();
}

// ---- the partial file ------------------------------------------------------------------------

namespace {

std::string system_message(int code) { return std::system_category().message(code); }

// The partial file, written at explicit offsets by several streams at once.
class PartFile {
public:
    PartFile() = default;
    PartFile(const PartFile&) = delete;
    PartFile& operator=(const PartFile&) = delete;
    ~PartFile() { close(); }

    // Creates it sparse (writes far into the file must not zero-fill what lies before) and
    // sized, or opens an existing one.
    std::expected<void, std::string> open(const fs::path& p, std::uint64_t size, bool create) {
#ifdef _WIN32
        h_ = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                         create ? CREATE_ALWAYS : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            h_ = nullptr;
            return util::failf("cannot open {}: {}", util::path_to_utf8(p), system_message(static_cast<int>(GetLastError())));
        }
        if (create) {
            DWORD ret = 0;
            DeviceIoControl(h_, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &ret, nullptr); // best effort (FAT)
            FILE_END_OF_FILE_INFO eof{};
            eof.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
            if (!SetFileInformationByHandle(h_, FileEndOfFileInfo, &eof, sizeof(eof))) {
                return util::failf("cannot size {}: {}", util::path_to_utf8(p),
                                   system_message(static_cast<int>(GetLastError())));
            }
        }
#else
        fd_ = ::open(p.c_str(), O_RDWR | (create ? O_CREAT | O_TRUNC : 0), 0644);
        if (fd_ < 0) return util::failf("cannot open {}: {}", util::path_to_utf8(p), system_message(errno));
        if (create && ftruncate(fd_, static_cast<off_t>(size)) != 0) {
            return util::failf("cannot size {}: {}", util::path_to_utf8(p), system_message(errno));
        }
#endif
        return {};
    }

    bool write_at(std::uint64_t offset, std::string_view data) {
        while (!data.empty()) {
#ifdef _WIN32
            OVERLAPPED ov{};
            ov.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFu);
            ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
            DWORD written = 0;
            const DWORD n = static_cast<DWORD>(std::min<std::size_t>(data.size(), 1u << 30));
            if (!WriteFile(h_, data.data(), n, &written, &ov) || written == 0) return false;
#else
            const ssize_t written = pwrite(fd_, data.data(), data.size(), static_cast<off_t>(offset));
            if (written <= 0) return false;
#endif
            offset += static_cast<std::uint64_t>(written);
            data.remove_prefix(static_cast<std::size_t>(written));
        }
        return true;
    }

    void flush() {
#ifdef _WIN32
        if (h_ != nullptr) FlushFileBuffers(h_);
#else
        if (fd_ >= 0) fsync(fd_);
#endif
    }

    // Modification time, nanoseconds since 1970 UTC.
    void set_mtime(std::int64_t unix_ns) {
#ifdef _WIN32
        const std::int64_t ticks = unix_ns / 100 + 116444736000000000LL; // 100 ns since 1601
        FILETIME ft{static_cast<DWORD>(ticks & 0xFFFFFFFF), static_cast<DWORD>(ticks >> 32)};
        SetFileTime(h_, nullptr, nullptr, &ft);
#else
        timespec times[2];
        times[0].tv_sec = 0;
        times[0].tv_nsec = UTIME_OMIT;
        times[1].tv_sec = static_cast<time_t>(unix_ns / 1'000'000'000);
        times[1].tv_nsec = static_cast<long>(unix_ns % 1'000'000'000);
        futimens(fd_, times);
#endif
    }

    void close() {
#ifdef _WIN32
        if (h_ != nullptr) CloseHandle(h_);
        h_ = nullptr;
#else
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
#endif
    }

private:
#ifdef _WIN32
    HANDLE h_ = nullptr;
#else
    int fd_ = -1;
#endif
};

std::chrono::seconds backoff(int attempt) {
    constexpr int steps[] = {2, 4, 8, 16, 30, 30};
    return std::chrono::seconds(steps[std::clamp(attempt - 1, 0, 5)]);
}

} // namespace

// ---- a download ------------------------------------------------------------------------------

Result run(const Download& d, Connections& connections, const std::function<void(const Progress&)>& on_progress,
           std::stop_token stop) {
    auto failed = [](std::string why) { return Result{Outcome::failed, std::move(why), {}}; };
    const Source& src = d.source;

    proc::RunOptions quiet;
    quiet.detached = true;
    quiet.stop = stop;
    auto stat_run = proc::run_capture(stat_argv(src), quiet);
    if (stop.stop_requested()) return Result{Outcome::stopped, {}, {}};
    if (!stat_run) return failed(stat_run.error());
    if (stat_run->exit_code != 0) return failed(server_ops::clean_rclone_error(stat_run->err));
    auto remote = parse_stat(stat_run->out);
    if (!remote) return failed(remote.error());
    if (remote->is_dir) return failed("this is a folder on the server; download it as a folder");

    Result result;
    const auto part = part_path(d.target);
    const auto state_file = state_path(d.target);
    std::optional<State> saved;
    if (auto text = util::read_file(state_file)) saved = state_from_json(*text);
    std::error_code ec;
    const bool reuse = saved && saved->rclone_remote == src.rclone_remote && saved->path == src.path &&
                       saved->size == remote->size && saved->modtime == remote->modtime &&
                       fs::file_size(part, ec) == remote->size && !ec;
    if (fs::exists(state_file, ec) && !reuse) {
        result.note = "the file changed on the server, or its partial copy was damaged; started over";
    }
    State state = reuse ? *saved
                        : new_state(src.rclone_remote, src.path, remote->size, remote->modtime,
                                    chunk_size_for(remote->size, connections.budget(d.server)));
    if (!reuse) discard(d.target);

    const auto remaining = state.size - state.bytes_done();
    if (auto space = fs::space(d.target.parent_path(), ec); !ec && space.available < remaining) {
        return failed(std::format("not enough free space in {}: {} needed, {} free",
                                  util::path_to_utf8(d.target.parent_path()), util::format_size(remaining),
                                  util::format_size(space.available)));
    }

    PartFile file;
    if (auto opened = file.open(part, state.size, !reuse); !opened) return failed(opened.error());
    auto save = [&](const State& s) { return server_ops::write_text(state_file, state_to_json(s)); };
    if (!reuse && !save(state)) return failed("cannot write " + util::path_to_utf8(state_file));

    // Shared between the streams, under `m`. Per range (same index as state.ranges, which only
    // ever grows at the end): being fetched, failures in a row, and when it may be retried.
    std::mutex m;
    std::condition_variable_any wake;
    std::vector<char> busy(state.ranges.size(), 0);
    std::vector<int> tries(state.ranges.size(), 0);
    std::vector<Clock::time_point> not_before(state.ranges.size(), Clock::time_point{});
    std::string error;
    std::stop_source inner; // ends all streams: a stop from outside, or a fatal error
    std::stop_callback forward(stop, [&] { inner.request_stop(); });
    const std::stop_token token = inner.get_token();

    // Under `m`: a free unfinished range, else a new one split off the busiest range. Sets
    // `retry_at` when only ranges that are backing off remain.
    auto next_range = [&](std::optional<Clock::time_point>& retry_at) -> std::optional<std::size_t> {
        const auto now = Clock::now();
        for (std::size_t r = 0; r < state.ranges.size(); ++r) {
            if (busy[r] || state.ranges[r].remaining() == 0) continue;
            if (not_before[r] <= now) return r;
            retry_at = retry_at ? std::min(*retry_at, not_before[r]) : not_before[r];
        }
        if (retry_at) return std::nullopt;
        std::optional<std::size_t> busiest;
        for (std::size_t r = 0; r < state.ranges.size(); ++r) {
            if (busy[r] && (!busiest || state.ranges[r].remaining() > state.ranges[*busiest].remaining())) busiest = r;
        }
        if (!busiest) return std::nullopt;
        auto tail = split(state, *busiest);
        if (tail) {
            busy.push_back(0);
            tries.push_back(0);
            not_before.push_back(Clock::time_point{});
        }
        return tail;
    };

    if (!d.joined) connections.join(d.server, d.id);
    auto stream = [&] {
        for (;;) {
            // A connection first: work found while waiting for one could be gone by then.
            if (!connections.acquire(d.server, d.id, token)) return;
            std::size_t i = 0;
            std::uint64_t offset = 0;
            std::uint64_t count = 0;
            {
                std::unique_lock lock(m);
                std::optional<Clock::time_point> retry_at;
                auto pick = token.stop_requested() ? std::nullopt : next_range(retry_at);
                if (!pick) {
                    lock.unlock();
                    connections.release(d.server, d.id);
                    if (!retry_at || token.stop_requested()) return; // nothing left for this stream
                    lock.lock();
                    wake.wait_until(lock, token, *retry_at, [] { return false; });
                    continue;
                }
                i = *pick;
                busy[i] = 1;
                offset = state.ranges[i].start + state.ranges[i].done;
                count = state.ranges[i].remaining();
            }
            std::uint64_t got = 0;
            bool write_failed = false;
            bool yielded = false;  // gave the connection to another download mid-range
            bool restarted = false; // the speed limit changed: start again with fitting flags
            // The limit's setting this stream starts under; its read-ahead depends on it.
            const std::uint64_t limit_generation = d.limiter != nullptr ? d.limiter->generation() : 0;
            const bool limited = d.limiter != nullptr && d.limiter->rate() > 0;
            proc::RunOptions opts;
            opts.detached = true;
            opts.stop = token;
            auto run = proc::run_piped(cat_argv(src, offset, count, limited), [&](std::string_view data) {
                // The speed limit: while this waits, rclone's pipe fills and its SSH reads pause.
                if (d.limiter != nullptr && !d.limiter->acquire(data.size(), token)) return false;
                {
                    // Under the lock: the range may have been shortened by a split meanwhile.
                    std::lock_guard lock(m);
                    if (token.stop_requested()) return false;
                    Range& r = state.ranges[i];
                    data = data.substr(0, static_cast<std::size_t>(std::min<std::uint64_t>(data.size(), r.remaining())));
                    if (!data.empty() && !file.write_at(r.start + r.done, data)) {
                        write_failed = true;
                        return false;
                    }
                    r.done += data.size();
                    got += data.size();
                    if (r.remaining() == 0) return false;
                }
                // A download that just joined this server gets its share now, not when this
                // range ends; what was written stays and the range is picked up again later.
                if (connections.yield_if_over_share(d.server, d.id)) {
                    yielded = true;
                    return false;
                }
                if (d.limiter != nullptr && d.limiter->generation() != limit_generation) {
                    restarted = true;
                    return false;
                }
                return true;
            }, opts);
            // A refusal before any data arrived means this server wants fewer connections.
            const bool refused = run && run->exit_code != 0 && got == 0 && !yielded && !restarted &&
                                 !token.stop_requested() &&
                                 budget::classify_failure(run->err) == budget::Failure::refused;
            if (refused) connections.refused(d.server);
            if (!yielded) connections.release(d.server, d.id);

            std::lock_guard lock(m);
            busy[i] = 0;
            wake.notify_all();
            if (token.stop_requested()) return;
            if (yielded || restarted) continue; // not a failure; the range goes on from its bytes
            if (write_failed) {
                error = "cannot write " + util::path_to_utf8(part) + " (disk full?)";
                inner.request_stop();
                return;
            }
            if (state.ranges[i].remaining() == 0) continue;
            if (refused) {
                not_before[i] = Clock::now() + std::chrono::seconds(1);
                continue;
            }
            if (got > 0) tries[i] = 0; // it made progress: only repeated failures in a row count
            if (++tries[i] > max_retries) {
                error = !run ? run.error()
                        : run->exit_code == 0 ? "the server sent less data than expected; was the file changed?"
                                              : server_ops::clean_rclone_error(run->err);
                inner.request_stop();
                return;
            }
            not_before[i] = Clock::now() + backoff(tries[i]);
        }
    };

    // As many streams as the budget allows (splitting makes work for all of them); the pool
    // decides how many actually run at once.
    const std::size_t stream_count =
        state.complete() ? 0 : std::clamp<std::size_t>(static_cast<std::size_t>(connections.budget(d.server)), 1, 16);
    std::atomic<std::size_t> running{stream_count};
    std::vector<std::jthread> streams;
    streams.reserve(stream_count);
    for (std::size_t s = 0; s < stream_count; ++s) {
        streams.emplace_back([&] {
            stream();
            --running;
        });
    }

    // Report progress and save state until the streams are done.
    std::deque<std::pair<Clock::time_point, std::uint64_t>> samples;
    auto last_save = Clock::now();
    auto snapshot = [&] {
        std::lock_guard lock(m);
        return state;
    };
    auto checkpoint = [&] {
        const State s = snapshot(); // counted bytes were written before they were counted
        file.flush();
        save(s);
    };
    while (running.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const auto now = Clock::now();
        std::uint64_t bytes = 0;
        {
            std::lock_guard lock(m);
            bytes = state.bytes_done();
        }
        samples.emplace_back(now, bytes);
        while (samples.size() > 1 && now - samples.front().first > std::chrono::seconds(5)) samples.pop_front();
        Progress p;
        p.bytes = bytes;
        p.total = state.size;
        const auto span = std::chrono::duration<double>(now - samples.front().first).count();
        if (span > 0.5) p.speed = static_cast<double>(bytes - samples.front().second) / span;
        if (p.speed > 0 && p.total > p.bytes) p.eta = static_cast<double>(p.total - p.bytes) / p.speed;
        p.connections = connections.active(d.server, d.id);
        if (on_progress) on_progress(p);
        if (now - last_save >= std::chrono::seconds(3)) {
            checkpoint();
            last_save = now;
        }
    }
    streams.clear(); // join
    if (!d.joined) connections.leave(d.server, d.id);
    checkpoint();

    if (stop.stop_requested()) return Result{Outcome::stopped, {}, result.note};
    if (!error.empty()) return Result{Outcome::failed, error, result.note};
    if (!state.complete()) return Result{Outcome::failed, "the download ended incomplete", result.note};

    if (auto t = parse_rfc3339(state.modtime)) file.set_mtime(*t);
    file.close();
    if (fs::file_size(part, ec) != state.size || ec) {
        return Result{Outcome::failed, "the downloaded file has the wrong size", result.note};
    }
    fs::rename(part, d.target, ec); // replaces an older copy, as rclone copyto did
    if (ec) return Result{Outcome::failed, std::format("cannot rename to {}: {}", util::path_to_utf8(d.target), ec.message()), result.note};
    fs::remove(state_file, ec);
    if (on_progress) on_progress(Progress{state.size, state.size, 0, std::nullopt, 0});
    result.outcome = Outcome::done;
    return result;
}

} // namespace grab::fetch
