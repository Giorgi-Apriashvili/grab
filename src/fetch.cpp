#include "fetch.hpp"

#include "json.hpp"
#include "process.hpp"
#include "rclone.hpp"
#include "rclone_encoding.hpp"
#include "server_ops.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <format>
#include <limits>
#include <map>
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

std::optional<std::size_t> split(State& s, std::size_t i, std::uint64_t asked, std::uint64_t min_piece) {
    Range& r = s.ranges[i];
    constexpr std::uint64_t align = 64 * 1024;
    const std::uint64_t open = r.remaining() > asked ? r.remaining() - asked : 0;
    const std::uint64_t half = open / 2 / align * align;
    if (half < min_piece || r.remaining() - half < min_piece) return std::nullopt;
    const Range tail{r.start + r.length - half, half, 0};
    r.length -= half; // the stream on `r` stops at the new end
    s.ranges.push_back(tail);
    return s.ranges.size() - 1;
}

bool same_modtime(std::string_view a, std::string_view b) {
    if (a == b) return true;
    const auto x = util::parse_rfc3339(a);
    const auto y = util::parse_rfc3339(b);
    return x && y && *x == *y;
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

bool up_to_date(std::uint64_t local_size, std::int64_t local_mtime, std::uint64_t size,
                std::optional<std::int64_t> modtime) {
    constexpr std::int64_t slack = 2'000'000'000; // FAT keeps times to 2 s
    return local_size == size && (!modtime || local_mtime >= *modtime - slack);
}

bool up_to_date(const fs::path& local, std::uint64_t size, std::string_view modtime) {
    std::error_code ec;
    if (!fs::is_regular_file(local, ec)) return false;
    const auto local_size = fs::file_size(local, ec);
    if (ec) return false;
    const auto written = fs::last_write_time(local, ec);
    if (ec) return false;
    const auto since_1970 = std::chrono::clock_cast<std::chrono::system_clock>(written).time_since_epoch();
    return up_to_date(local_size, std::chrono::duration_cast<std::chrono::nanoseconds>(since_1970).count(), size,
                      util::parse_rfc3339(modtime));
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

PartialFiles partial_files(const fs::path& target, int slot) {
    PartialFiles f{target, {}};
    if (slot > 1) f.part += std::format(".{}", slot);
    f.part += ".grabpart";
    f.record = f.part;
    f.record += ".json";
    return f;
}

namespace {

bool is_reserved(const Reserved& reserved, const PartialFiles& f) {
    return reserved && (reserved(f.part) || reserved(f.record));
}

// Deletes a partial download grab made: the data file while it has the size grab gave it (what
// is there otherwise isn't grab's), then the record.
void discard_own(const Partial& p) {
    std::error_code ec;
    if (fs::is_regular_file(p.files.part, ec) && fs::file_size(p.files.part, ec) == p.state.size && !ec &&
        !fs::remove(p.files.part, ec) && ec) {
        return; // in use: the record stays with it
    }
    fs::remove(p.files.record, ec);
}

} // namespace

std::optional<Partial> find_partial(const fs::path& target, std::string_view remote_path, const Reserved& reserved,
                                    std::string_view rclone_remote) {
    constexpr std::uintmax_t record_max = 1 << 20; // a record is a few KiB; no reading anything big
    for (int slot = 1; slot <= max_slot; ++slot) {
        auto f = partial_files(target, slot);
        if (is_reserved(reserved, f)) continue;
        std::error_code ec;
        if (!fs::is_regular_file(f.record, ec) || fs::file_size(f.record, ec) > record_max || ec) continue;
        auto text = util::read_file(f.record);
        if (!text) continue;
        auto state = state_from_json(*text);
        if (state && state->path == remote_path && (rclone_remote.empty() || state->rclone_remote == rclone_remote)) {
            return Partial{.files = std::move(f), .state = std::move(*state)};
        }
    }
    return std::nullopt;
}

std::optional<PartialFiles> free_partial(const fs::path& target, const Reserved& reserved) {
    for (int slot = 1; slot <= max_slot; ++slot) {
        auto f = partial_files(target, slot);
        if (is_reserved(reserved, f)) continue;
        std::error_code ec;
        if (!fs::exists(fs::symlink_status(f.part, ec)) && !fs::exists(fs::symlink_status(f.record, ec))) return f;
    }
    return std::nullopt;
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> saved_progress(const fs::path& target,
                                                                      std::string_view remote_path,
                                                                      const Reserved& reserved,
                                                                      std::string_view rclone_remote) {
    const auto p = find_partial(target, remote_path, reserved, rclone_remote);
    if (!p) return std::nullopt;
    return std::pair{p->state.bytes_done(), p->state.size};
}

void discard(const fs::path& target, std::string_view remote_path, const Reserved& reserved,
             std::string_view rclone_remote) {
    if (const auto p = find_partial(target, remote_path, reserved, rclone_remote)) discard_own(*p);
}

bool move_partial(const fs::path& from, const fs::path& to, std::string_view remote_path, std::string_view rclone_remote) {
    const auto p = find_partial(from, remote_path, {}, rclone_remote);
    if (!p) return false;
    const auto dest = partial_files(to, 1);
    std::error_code ec;
    if (fs::exists(fs::symlink_status(dest.part, ec)) || fs::exists(fs::symlink_status(dest.record, ec))) return false;
    fs::rename(p->files.part, dest.part, ec);
    if (ec) return false;
    fs::rename(p->files.record, dest.record, ec);
    if (ec) {
        fs::rename(dest.part, p->files.part, ec); // back, so the record still has its data
        return false;
    }
    return true;
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

void Connections::join(const std::string& server, int id, int weight) {
    std::lock_guard lock(mutex_);
    pool(server).add_user(id, weight);
    changed_.notify_all();
}

void Connections::set_weight(const std::string& server, int id, int weight) {
    std::lock_guard lock(mutex_);
    pool(server).set_weight(id, weight);
    changed_.notify_all(); // a raised share may start streams now; a lowered one yields at the next read
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

int Connections::reserve(const std::string& server, int id, std::stop_token stop, int at_most,
                         std::chrono::milliseconds settle) {
    if (!acquire(server, id, stop)) return 0;
    std::unique_lock lock(mutex_);
    auto& p = pool(server);
    int got = 1;
    const auto deadline = budget::ServerPool::Clock::now() + settle;
    bool counted = false; // waiting, so downloads above their shares hand connections back
    for (;;) {
        const int wanted = std::min(p.share(id), at_most);
        const auto now = budget::ServerPool::Clock::now();
        while (got < wanted && p.total_active() < p.budget()) {
            p.started(id, now); // rclone staggers its own connections
            ++got;
        }
        if (got >= wanted || now >= deadline || stop.stop_requested()) break;
        if (!counted) {
            p.waiting(id, +1);
            counted = true;
        }
        changed_.wait_until(lock, stop, deadline, [] { return false; });
    }
    if (counted) p.waiting(id, -1);
    return got;
}

int Connections::share(const std::string& server, int id) {
    std::lock_guard lock(mutex_);
    return pool(server).share(id);
}

void Connections::release(const std::string& server, int id, int count, bool worked) {
    std::lock_guard lock(mutex_);
    auto& p = pool(server);
    for (int i = 0; i < count; ++i) p.finished(id);
    if (worked) p.working(id, -count);
    changed_.notify_all();
}

void Connections::working(const std::string& server, int id, int count) {
    std::lock_guard lock(mutex_);
    pool(server).working(id, count);
}

bool Connections::unbalanced(const std::string& server, int id, int held, int at_most) {
    std::lock_guard lock(mutex_);
    auto& p = pool(server);
    if (p.should_yield(id)) return true;
    const int share = std::min(p.share(id), at_most);
    const int free = p.budget() - p.total_active();
    return held < share && std::min(share, held + free) >= held + 2;
}

bool Connections::yield_if_over_share(const std::string& server, int id) {
    std::lock_guard lock(mutex_);
    auto& p = pool(server);
    if (!p.should_yield(id)) return false;
    p.finished(id);
    p.working(id, -1); // it yields mid-transfer, after data
    changed_.notify_all();
    return true;
}

void Connections::refused(const std::string& server) {
    int lowered = 0;
    {
        std::lock_guard lock(mutex_);
        ++refusals_[server];
        auto& p = pool(server);
        if (p.refused()) lowered = p.budget();
    }
    if (lowered > 0 && on_learned_) on_learned_(server, lowered);
}

std::uint64_t Connections::refusals(const std::string& server) {
    std::lock_guard lock(mutex_);
    const auto it = refusals_.find(server);
    return it == refusals_.end() ? 0 : it->second;
}

int Connections::active(const std::string& server, int id) {
    std::lock_guard lock(mutex_);
    return pool(server).active(id);
}

int Connections::budget(const std::string& server) {
    std::lock_guard lock(mutex_);
    return pool(server).budget();
}

int Connections::new_id() { return --next_id_; }

Listing::Listing(Connections& connections, std::string server, std::stop_token stop, int at_most)
    : connections_(connections), server_(std::move(server)), id_(connections.new_id()) {
    connections_.join(server_, id_, 4);
    held_ = connections_.reserve(server_, id_, stop, at_most, std::chrono::seconds(2));
}

Listing::~Listing() {
    if (held_ > 0) connections_.release(server_, id_, held_);
    connections_.leave(server_, id_);
}

std::vector<std::string> Listing::flags() const {
    const std::string n = std::to_string(std::max(held_, 1));
    return {"--checkers", n, "--sftp-connections", n};
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
    // sized, only where nothing exists yet; or opens grab's existing one.
    std::expected<void, std::string> open(const fs::path& p, std::uint64_t size, bool create) {
#ifdef _WIN32
        h_ = CreateFileW(p.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                         create ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
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
        fd_ = ::open(p.c_str(), O_RDWR | (create ? O_CREAT | O_EXCL : 0), 0644);
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

    // To the disk, so a record saved after it never claims bytes a power cut lost (review G71).
    [[nodiscard]] std::expected<void, std::string> flush() {
#ifdef _WIN32
        if (h_ != nullptr && !FlushFileBuffers(h_)) return util::fail(util::error_message(GetLastError()));
#else
        if (fd_ >= 0 && fsync(fd_) != 0) return util::fail(util::error_message(static_cast<unsigned long>(errno)));
#endif
        return {};
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

// Replaces grab's own record, never half written: the text goes to a new file of a random name
// beside it, on the disk before it moves over the record.
std::expected<void, std::string> write_record(const fs::path& record, const std::string& text) {
    return util::write_file_atomic(record, text);
}

// A new partial download in the first free slot: its record is created first, then the data
// file, each only where nothing exists yet.
std::expected<PartialFiles, std::string> claim(const fs::path& target, const State& state, const Reserved& reserved,
                                               PartFile& file) {
    auto f = free_partial(target, reserved);
    if (!f) {
        return util::failf("no free name for the partial download: {} and {} more names next to it are taken by other files",
                           util::path_to_utf8(partial_files(target, 1).part), max_slot - 1);
    }
    if (!util::write_new_file(f->record, state_to_json(state))) return util::fail("cannot write " + util::path_to_utf8(f->record));
    if (auto opened = file.open(f->part, state.size, true); !opened) {
        std::error_code ec;
        fs::remove(f->record, ec); // made just now
        return util::fail(opened.error());
    }
    return *f;
}

std::chrono::seconds backoff(int attempt) {
    constexpr int steps[] = {2, 4, 8, 16, 30, 30};
    return std::chrono::seconds(steps[std::clamp(attempt - 1, 0, 5)]);
}

// One look at the server's file, through either engine: what it is, or why that failed.
struct Look {
    std::optional<RemoteStat> stat;
    budget::Failure failure = budget::Failure::other;
    std::string error;
};

Look look_rclone(const Download& d, const proc::RunOptions& quiet, const std::stop_token& stop) {
    const auto argv = stat_argv(d.source);
    if (d.on_command) d.on_command(argv);
    auto run = proc::run_capture(argv, quiet);
    if (!run) return Look{std::nullopt, budget::Failure::other, run.error()};
    if (run->exit_code != 0) {
        const auto failure = stop.stop_requested() ? budget::Failure::other : budget::classify_failure(run->err);
        return Look{std::nullopt, failure,
                    name_not_utf8(d.source.path) ? std::string(not_utf8_error) : server_ops::clean_rclone_error(run->err)};
    }
    auto parsed = parse_stat(run->out);
    if (!parsed) return Look{std::nullopt, budget::Failure::fatal, parsed.error()};
    return Look{*parsed, {}, {}};
}

// "2023-05-06T11:18:12Z": SFTP gives whole seconds since 1970, in UTC.
std::string utc_rfc3339(std::uint32_t seconds) {
    return std::format("{:%FT%TZ}", std::chrono::sys_seconds{std::chrono::seconds{seconds}});
}

Look look_sftp(const Download& d, const std::stop_token& stop) {
    if (d.on_command) d.on_command({"sftp", "stat", d.source.path});
    Look look;
    std::optional<ssh::Pool::Lease> lease;
    try {
        lease = d.pool->acquire(stop);
        if (!lease) return look; // stopped
        auto a = sftp::stat((*lease)->sftp(), d.source.path);
        if (!a) {
            look.failure = budget::Failure::fatal;
            look.error = sftp::describe(a.error());
            return look;
        }
        look.stat = RemoteStat{a->size.value_or(0), a->mtime ? utc_rfc3339(*a->mtime) : std::string(), a->is_dir()};
    } catch (const ssh::Error& e) {
        look.failure = e.kind;
        look.error = e.what();
    } catch (const sftp::Broken& e) {
        if (lease) lease->broken();
        look.error = e.what();
    }
    return look;
}

// The download's place in its server's pool, from the size check on, left on every way out.
class Seat {
public:
    Seat(Connections& connections, const Download& d, int weight) : connections_(connections), d_(d) {
        if (!d.joined) connections_.join(d.server, d.id, weight);
    }
    Seat(const Seat&) = delete;
    Seat& operator=(const Seat&) = delete;
    ~Seat() { leave(); }
    void leave() {
        if (!d_.joined && !left_) connections_.leave(d_.server, d_.id);
        left_ = true;
    }

private:
    Connections& connections_;
    const Download& d_;
    bool left_ = false;
};

} // namespace

// ---- a download ------------------------------------------------------------------------------

Result run(const Download& d, Connections& connections, const std::function<void(const Progress&)>& on_progress,
           std::stop_token stop) {
    auto failed = [](std::string why) { return Result{Outcome::failed, std::move(why), {}}; };
    const Source& src = d.source;
    // Bytes that aren't UTF-8 can't reach rclone: said at once, not as "directory not found" (G17).
    if (!rclone_enc::valid_utf8(src.path)) return failed(std::string(not_utf8_error));
    // Past what Windows takes here, the partial download's record could never be written (G19).
    if (auto why = util::too_long(d.target, std::string_view(".2.grabpart.json").size())) return failed(std::move(*why));
    auto weight = [&] { return d.weight != nullptr ? d.weight->load() : 2; };
    Seat seat(connections, d, weight());

    proc::RunOptions quiet;
    quiet.detached = true;
    quiet.stop = stop;
    // The size check is a connection like the others: taken from the pool, and when the server
    // is full, asked again 1, 2, 4, 8, then 15 s apart rather than failing the file (review G193).
    std::optional<RemoteStat> remote;
    const auto stat_from = Clock::now();
    for (int attempt = 0;; ++attempt) {
        if (!connections.acquire(d.server, d.id, stop)) return Result{Outcome::stopped, {}, {}};
        Look look = d.pool ? look_sftp(d, stop) : look_rclone(d, quiet, stop);
        const bool refused = !look.stat && !stop.stop_requested() && look.failure == budget::Failure::refused;
        if (refused) connections.refused(d.server);
        connections.release(d.server, d.id);
        if (stop.stop_requested()) return Result{Outcome::stopped, {}, {}};
        if (look.stat) {
            remote = std::move(look.stat);
            break;
        }
        if (!refused || Clock::now() - stat_from >= refused_give_up) return failed(look.error);
        std::mutex wait_m;
        std::condition_variable_any wait_cv;
        std::unique_lock wait_lock(wait_m);
        wait_cv.wait_for(wait_lock, stop, std::min(std::chrono::seconds(1 << std::min(attempt, 4)), std::chrono::seconds(15)),
                         [] { return false; });
    }
    if (remote->is_dir) return failed("this is a folder on the server; download it as a folder");

    Result result;
    result.size = remote->size;
    result.modtime = remote->modtime;
    std::error_code ec;
    auto own = find_partial(d.target, src.path, d.reserved, src.rclone_remote);
    // A file there that is the server's already (same size, not older) is kept, also when a
    // partial download of it was left: that is then given up (review G75).
    if (up_to_date(d.target, remote->size, remote->modtime)) {
        if (own) discard_own(*own);
        if (on_progress) on_progress(Progress{remote->size, remote->size, 0, std::nullopt, 0});
        result.outcome = Outcome::done;
        result.present = true;
        return result;
    }
    // A different file there stays unless the user chose to replace it.
    auto blocked = [&] {
        std::error_code e;
        return !d.replace && fs::is_regular_file(d.target, e) && !up_to_date(d.target, remote->size, remote->modtime);
    };
    if (blocked()) {
        result.outcome = Outcome::exists;
        result.error = "a different file is already at " + util::path_to_utf8(d.target);
        return result;
    }
    // Either engine's partial goes on under the other: the same server, size and moment.
    const bool reuse = own && own->state.rclone_remote == src.rclone_remote && own->state.size == remote->size &&
                       same_modtime(own->state.modtime, remote->modtime) &&
                       fs::file_size(own->files.part, ec) == remote->size && !ec;
    if (own && !reuse) {
        result.note = "the file changed on the server, or its partial copy was damaged; started over";
        discard_own(*own);
    }
    State state = reuse ? own->state
                        : new_state(src.rclone_remote, src.path, remote->size, remote->modtime,
                                    chunk_size_for(remote->size, connections.budget(d.server)));

    const auto remaining = state.size - state.bytes_done();
    if (auto space = fs::space(d.target.parent_path(), ec); !ec && space.available < remaining) {
        return failed(std::format("not enough free space in {}: {} needed, {} free",
                                  util::path_to_utf8(d.target.parent_path()), util::format_size(remaining),
                                  util::format_size(space.available)));
    }

    PartFile file;
    PartialFiles files;
    if (reuse) {
        files = own->files;
        if (auto opened = file.open(files.part, state.size, false); !opened) return failed(opened.error());
    } else {
        auto claimed = claim(d.target, state, d.reserved, file);
        if (!claimed) return failed(claimed.error());
        files = std::move(*claimed);
    }
    const fs::path& part = files.part;
    auto save = [&](const State& s) { return write_record(files.record, state_to_json(s)); };

    // Shared between the streams, under `m`. Per range (same index as state.ranges, which only
    // ever grows at the end): being fetched, failures in a row, and when it may be retried.
    std::mutex m;
    std::condition_variable_any wake;
    std::vector<char> busy(state.ranges.size(), 0);
    std::vector<int> tries(state.ranges.size(), 0);
    std::vector<Clock::time_point> not_before(state.ranges.size(), Clock::time_point{});
    // grab's engine: how far each busy range's stream has asked the server for (a file offset;
    // 0 when nothing past its done bytes), which a split leaves with it.
    std::vector<std::uint64_t> asked_to(state.ranges.size(), 0);
    auto asked_past_done = [&](std::size_t r) -> std::uint64_t {
        const std::uint64_t at = state.ranges[r].start + state.ranges[r].done;
        return busy[r] && asked_to[r] > at ? asked_to[r] - at : 0;
    };
    // While the server refuses this download and no data comes (it is full with something else's
    // connections): since when, how many times in a row, and until when no stream may try. Then
    // one stream at a time asks, 1, 2, 4, 8, then 15 s apart (review G9).
    std::optional<Clock::time_point> refused_since;
    int refused_streak = 0;
    Clock::time_point hold_until{};
    std::string error;
    std::stop_source inner; // ends all streams: a stop from outside, or a fatal error
    std::stop_callback forward(stop, [&] { inner.request_stop(); });
    const std::stop_token token = inner.get_token();

    // Under `m`: a free unfinished range, else a new one split off the busiest range. Sets
    // `retry_at` when only ranges that are backing off remain.
    auto next_range = [&](std::optional<Clock::time_point>& retry_at) -> std::optional<std::size_t> {
        const auto now = Clock::now();
        if (refused_since) {
            if (now < hold_until) {
                retry_at = hold_until;
                return std::nullopt;
            }
            if (std::ranges::find(busy, 1) != busy.end()) { // one asks at a time
                retry_at = now + std::chrono::milliseconds(250);
                return std::nullopt;
            }
        }
        for (std::size_t r = 0; r < state.ranges.size(); ++r) {
            if (busy[r] || state.ranges[r].remaining() == 0) continue;
            if (not_before[r] <= now) return r;
            retry_at = retry_at ? std::min(*retry_at, not_before[r]) : not_before[r];
        }
        if (retry_at) return std::nullopt;
        // The busy range with the most bytes nobody asked for yet.
        auto open = [&](std::size_t r) { return state.ranges[r].remaining() - asked_past_done(r); };
        std::optional<std::size_t> busiest;
        for (std::size_t r = 0; r < state.ranges.size(); ++r) {
            if (busy[r] && (!busiest || open(r) > open(*busiest))) busiest = r;
        }
        if (!busiest) return std::nullopt;
        auto tail = fetch::split(state, *busiest, asked_past_done(*busiest), d.pool ? min_split_grab : min_split);
        if (tail) {
            busy.push_back(0);
            tries.push_back(0);
            not_before.push_back(Clock::time_point{});
            asked_to.push_back(0);
        }
        return tail;
    };

    // Connections this file's streams hold, against Download::streams_cap.
    std::atomic<int> holding{0};

    // What one go at a range came to.
    struct Attempt {
        std::uint64_t got = 0;
        bool worked = false;       // counted as working in the pool, from its first data
        bool write_failed = false;
        bool yielded = false;      // gave the connection to another download mid-range
        bool restarted = false;    // rclone: the speed limit went on or off, so start again with fitting flags
        bool gave_way = false;     // over its part of the folder's connections: one for another file
        std::optional<budget::Failure> failure; // it ended in an error: refused, fatal or other
        std::string error;
    };
    // Bytes of range `i` that arrived, written where they belong. False: stop, as the range is
    // done (a split may have shortened it meanwhile) or the write failed (`a.write_failed`).
    auto put = [&](std::size_t i, std::string_view data, Attempt& a) {
        std::lock_guard lock(m);
        if (token.stop_requested()) return false;
        Range& r = state.ranges[i];
        data = data.substr(0, static_cast<std::size_t>(std::min<std::uint64_t>(data.size(), r.remaining())));
        if (!data.empty() && !file.write_at(r.start + r.done, data)) {
            a.write_failed = true;
            return false;
        }
        r.done += data.size();
        a.got += data.size();
        refused_since.reset(); // the server takes this download's connections
        refused_streak = 0;
        if (!a.worked && !data.empty()) {
            a.worked = true;
            connections.working(d.server, d.id);
        }
        return r.remaining() > 0;
    };
    // Whether the stream hands its connection over now; sets `a.yielded` or `a.gave_way`. What it
    // wrote stays, and the range is picked up again later.
    auto step_aside = [&](Attempt& a) {
        // A download that just joined this server gets its share now, not when this range ends.
        if (connections.yield_if_over_share(d.server, d.id)) {
            a.yielded = true;
            return true;
        }
        // Another file of the folder started: this one keeps only its even part (taken off here,
        // so its other streams don't all give theirs up at once).
        if (d.streams_cap) {
            const int cap = d.streams_cap();
            int h = holding.load();
            while (h > cap && !holding.compare_exchange_weak(h, h - 1)) {
            }
            if (h > cap) {
                a.gave_way = true;
                return true;
            }
        }
        return false;
    };

    // Range `i` from `offset` (its first byte not on disk), `count` bytes, as one rclone cat.
    auto rclone_range = [&](std::size_t i, std::uint64_t offset, std::uint64_t count, Attempt& a) {
        // Whether a speed limit is on as this stream starts: its read-ahead depends on that,
        // not on the limit's value, so only switching it on or off restarts the stream (G76).
        const bool limited = d.limiter != nullptr && d.limiter->rate() > 0;
        proc::RunOptions opts;
        opts.detached = true;
        opts.stop = token;
        const auto cat = cat_argv(src, offset, count, limited);
        if (d.on_command) d.on_command(cat);
        auto run = proc::run_piped(cat, [&](std::string_view chunk) {
            // Under a speed limit, in slices of 64 KiB: bytes pass evenly rather than a pipe's
            // worth (up to 1 MiB) at a time, and no slice waits for a full bucket, whose excess
            // a late wake-up would lose (the limit came out 5% short).
            const std::size_t slice = limited ? std::size_t{64} << 10 : chunk.size();
            while (!chunk.empty()) {
                std::string_view data = chunk.substr(0, slice);
                chunk.remove_prefix(data.size());
                // The speed limit: while this waits, rclone's pipe fills and its SSH reads pause.
                if (d.limiter != nullptr && !d.limiter->acquire(data.size(), token, d.id, weight())) return false;
                if (!put(i, data, a) || step_aside(a)) return false;
                if (d.limiter != nullptr && (d.limiter->rate() > 0) != limited) {
                    a.restarted = true;
                    return false;
                }
            }
            return true;
        }, opts);
        if (a.write_failed || a.yielded || a.restarted || a.gave_way || token.stop_requested()) return;
        if (!run) {
            a.failure = budget::Failure::other;
            a.error = run.error();
        } else if (run->exit_code != 0) {
            // A failure before any data arrived says why the connection failed: the server is
            // full (it wants fewer connections), or trying again can't help (review G9).
            a.failure = a.got == 0 ? budget::classify_failure(run->err) : budget::Failure::other;
            a.error = server_ops::clean_rclone_error(run->err);
        } else {
            a.failure = budget::Failure::other; // only counts when the range isn't done
            a.error = "the server sent less data than expected; was the file changed?";
        }
    };

    // Range `i` from `offset` through grab's engine: a connection borrowed from the pool, READs of
    // exact bounds pipelined (grab_depth in flight), each asked for up to the range's end as it
    // is then (a split for an idle stream takes only bytes not asked for yet), the speed limit
    // taken before each READ so the network follows it, data written in order.
    auto sftp_range = [&](std::size_t i, std::uint64_t offset, Attempt& a) {
        std::optional<ssh::Pool::Lease> lease;
        try {
            lease = d.pool->acquire(token);
        } catch (const ssh::Error& e) {
            a.failure = e.kind;
            a.error = e.what();
            return;
        }
        if (!lease) return; // stopped
        sftp::Client& c = (*lease)->sftp();
        try {
            auto handle = sftp::open_file(c, src.path);
            if (!handle) {
                const auto code = handle.error().code;
                a.failure = code == sftp::Code::no_such_file || code == sftp::Code::permission_denied
                                ? budget::Failure::fatal
                                : budget::Failure::other;
                a.error = sftp::describe(handle.error());
                return;
            }
            std::uint64_t next = offset;    // the next byte to ask for
            std::uint64_t deliver = offset; // the next byte to write
            std::map<std::uint64_t, std::string> early; // arrived before the bytes ahead of them
            int in_flight = 0;
            bool asking = true;  // false: no more READs (the range is done, the stream steps aside, a failure)
            bool writing = true; // false: a write failed; what still comes is dropped
            std::optional<sftp::Status> read_failed;
            // Stepping aside ends the stream now, its connection closed rather than drained: up to
            // 16 MiB is still on its way, and the download waiting for the connection (its permission
            // just handed over) would wait for that too, the pool being at the server's budget.
            bool stepped_aside = false;
            auto hand_over = [&](std::string_view data) {
                if (!writing || stepped_aside) return;
                if (!put(i, data, a)) {
                    asking = false;
                    writing = !a.write_failed;
                } else if (asking && step_aside(a)) {
                    asking = false;
                    stepped_aside = true;
                }
            };
            std::function<void(std::uint64_t, std::uint32_t)> send = [&](std::uint64_t at, std::uint32_t n) {
                ++in_flight;
                c.read(*handle, at, n, [&, at, n](std::expected<std::string_view, sftp::Status> r) {
                    --in_flight;
                    if (!r) {
                        if (!read_failed) read_failed = std::move(r.error());
                        asking = false;
                        return;
                    }
                    const auto got = static_cast<std::uint32_t>(r->size());
                    if (got < n) send(at + got, n - got); // a short read: the rest at once, exactly
                    if (at != deliver) {
                        early.emplace(at, std::string(*r));
                        return;
                    }
                    hand_over(*r);
                    deliver += got;
                    for (auto it = early.begin(); it != early.end() && it->first == deliver; it = early.erase(it)) {
                        hand_over(it->second);
                        deliver += it->second.size();
                    }
                });
            };
            // Under a speed limit, READs no bigger than an eighth of a second's worth, so data
            // comes evenly; otherwise as big as servers take.
            auto read_size = [&]() -> std::uint64_t {
                const std::uint64_t rate = d.limiter != nullptr ? d.limiter->rate() : 0;
                return rate == 0 ? sftp::max_read : std::clamp<std::uint64_t>(rate / 8, 32 * 1024, sftp::max_read);
            };
            for (;;) {
                // Under a limit, one READ at a time: it waits for its tokens (its turn among the
                // download's other streams), and a reply already here would otherwise wait unread
                // through the next turns, arriving on time but written in bursts. One READ is an
                // eighth of a second of the limit's worth, so a connection still carries it all.
                const int depth = d.limiter != nullptr && d.limiter->rate() > 0 ? 1 : grab_depth;
                while (asking && in_flight < depth && !token.stop_requested()) {
                    std::uint64_t n = 0;
                    {
                        std::lock_guard lock(m);
                        const Range& r = state.ranges[i];
                        const std::uint64_t end = r.start + r.length;
                        if (next < end) {
                            n = std::min(read_size(), end - next);
                            asked_to[i] = next + n;
                        }
                    }
                    if (n == 0) {
                        asking = false;
                        break;
                    }
                    if (d.limiter != nullptr && !d.limiter->acquire(n, token, d.id, weight())) break;
                    send(next, static_cast<std::uint32_t>(n));
                    next += n;
                }
                if (token.stop_requested() || stepped_aside || in_flight == 0) break;
                c.pump();
            }
            if (token.stop_requested() || stepped_aside) {
                lease->broken(); // its replies still on the way: closed, not waited for
                return;
            }
            if (read_failed) {
                a.failure = budget::Failure::other; // only counts when the range isn't done
                a.error = read_failed->code == sftp::Code::eof
                              ? "the server sent less data than expected; was the file changed?"
                              : sftp::describe(*read_failed);
            }
            // A handle that won't close leaves nothing behind that matters here.
            [[maybe_unused]] auto closed = sftp::close_file(c, *handle);
        } catch (const sftp::Broken& e) {
            lease->broken();
            if (!a.write_failed) {
                a.failure = budget::Failure::other;
                a.error = e.what();
            }
        }
    };

    auto stream = [&] {
        for (;;) {
            // No more than its part of a folder's connections: a place under it is taken before the
            // connection, so its streams starting together can't all pass; else it waits.
            int h = holding.load();
            const int cap = d.streams_cap ? d.streams_cap() : std::numeric_limits<int>::max();
            while (h < cap && !holding.compare_exchange_weak(h, h + 1)) {
            }
            if (h >= cap) {
                std::unique_lock lock(m);
                wake.wait_until(lock, token, Clock::now() + std::chrono::milliseconds(250), [] { return false; });
                if (token.stop_requested()) return;
                continue;
            }
            // A connection first: work found while waiting for one could be gone by then.
            if (!connections.acquire(d.server, d.id, token)) {
                --holding;
                return;
            }
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
                    --holding;
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
            Attempt a;
            if (d.pool) {
                if (d.on_command) {
                    d.on_command({"sftp", "read", src.path, "--offset", std::to_string(offset), "--count",
                                  std::to_string(count)});
                }
                sftp_range(i, offset, a);
            } else {
                rclone_range(i, offset, count, a);
            }
            const bool refused = a.failure == budget::Failure::refused && !token.stop_requested();
            if (refused) connections.refused(d.server);
            if (!a.yielded) connections.release(d.server, d.id, 1, a.worked);
            if (!a.gave_way) --holding; // given up, it was taken off already

            std::lock_guard lock(m);
            busy[i] = 0;
            asked_to[i] = 0;
            wake.notify_all();
            if (token.stop_requested()) return;
            if (a.write_failed) {
                error = "cannot write " + util::path_to_utf8(part) + " (disk full?)";
                inner.request_stop();
                return;
            }
            if (a.yielded || a.restarted || a.gave_way) continue; // not a failure; the range goes on from its bytes
            if (a.failure == budget::Failure::fatal) {
                error = a.error;
                inner.request_stop();
                return;
            }
            if (state.ranges[i].remaining() == 0) continue;
            if (refused) {
                const auto now = Clock::now();
                if (!refused_since) refused_since = now;
                if (now - *refused_since >= refused_give_up) {
                    error = "the server keeps refusing connections: " + a.error;
                    inner.request_stop();
                    return;
                }
                hold_until = now + std::min(std::chrono::seconds(1 << std::min(refused_streak++, 4)), std::chrono::seconds(15));
                not_before[i] = now + std::chrono::seconds(1); // also while others' data keeps coming
                continue;
            }
            if (a.got > 0) tries[i] = 0; // it made progress: only repeated failures in a row count
            if (++tries[i] > max_retries) {
                error = a.error.empty() ? "the server sent less data than expected; was the file changed?" : a.error;
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
        // Saved only once those bytes are on the disk; one that can't get there is a write error.
        if (auto flushed = file.flush(); !flushed) {
            std::lock_guard lock(m);
            if (error.empty()) error = std::format("cannot write {}: {}", util::path_to_utf8(part), flushed.error());
            inner.request_stop();
            return;
        }
        // Progress that can't be saved would be lost at the next stop: said, not carried on as if
        // it were (review G70).
        if (auto saved = save(s); !saved) {
            std::lock_guard lock(m);
            if (error.empty()) error = std::format("the progress can't be saved: {}", saved.error());
            inner.request_stop();
        }
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
    seat.leave();
    checkpoint();

    if (stop.stop_requested()) return Result{Outcome::stopped, {}, result.note};
    if (!error.empty()) return Result{Outcome::failed, error, result.note};
    if (!state.complete()) return Result{Outcome::failed, "the download ended incomplete", result.note};

    if (auto t = util::parse_rfc3339(state.modtime)) file.set_mtime(*t);
    file.close();
    if (fs::file_size(part, ec) != state.size || ec) {
        return Result{Outcome::failed, "the downloaded file has the wrong size", result.note};
    }
    if (blocked()) { // one appeared meanwhile: the complete download waits for the user's answer
        result.outcome = Outcome::exists;
        result.error = "a different file is already at " + util::path_to_utf8(d.target);
        return result;
    }
    fs::rename(part, d.target, ec); // replaces an older copy, as rclone copyto did
    if (ec) return Result{Outcome::failed, std::format("cannot rename to {}: {}", util::path_to_utf8(d.target), ec.message()), result.note};
    fs::remove(files.record, ec);
    if (on_progress) on_progress(Progress{state.size, state.size, 0, std::nullopt, 0});
    result.outcome = Outcome::done;
    return result;
}

} // namespace grab::fetch
