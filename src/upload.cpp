#include "upload.hpp"

#include "existing.hpp"
#include "json.hpp"
#include "rclone_encoding.hpp"
#include "sftp.hpp"
#include "speed.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"
#include "transfer.hpp"
#include "util.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <format>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace grab::upload {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

// ---- the local file ----------------------------------------------------------------------------

namespace {

// A file's size and modification time (nanoseconds since 1970).
struct Stamp {
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
    bool operator==(const Stamp&) const = default;
};

// A file as it was looked at: its stamp, and which file it is (its volume and index, or device and
// inode).
struct Seen {
    Stamp stamp;
    std::uint64_t volume = 0;
    std::uint64_t index = 0;
    [[nodiscard]] bool same_file(const Seen& other) const { return volume == other.volume && index == other.index; }
};

#ifdef _WIN32
std::int64_t unix_ns(const FILETIME& ft) {
    const auto ticks = static_cast<std::int64_t>((std::uint64_t{ft.dwHighDateTime} << 32) | ft.dwLowDateTime);
    return (ticks - 116444736000000000LL) * 100; // 100 ns since 1601
}

std::optional<Seen> seen_of(HANDLE h) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h, &info)) return std::nullopt;
    return Seen{.stamp = Stamp{.size = (std::uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow,
                               .mtime_ns = unix_ns(info.ftLastWriteTime)},
                .volume = info.dwVolumeSerialNumber,
                .index = (std::uint64_t{info.nFileIndexHigh} << 32) | info.nFileIndexLow};
}

// The file at `p` now, through a link at its end as opening it goes (GetFileAttributesExW would
// give the link's own size and time); none there, or one being deleted: nullopt.
std::optional<Seen> seen_of(const fs::path& p) {
    const HANDLE h = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    auto seen = seen_of(h);
    CloseHandle(h);
    return seen;
}
#else
std::optional<Seen> seen_of(const struct stat& st) {
    return Seen{.stamp = Stamp{.size = static_cast<std::uint64_t>(st.st_size),
                               .mtime_ns = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec},
                .volume = static_cast<std::uint64_t>(st.st_dev),
                .index = static_cast<std::uint64_t>(st.st_ino)};
}

std::optional<Seen> seen_of(int fd) {
    struct stat st {};
    if (fstat(fd, &st) != 0) return std::nullopt;
    return seen_of(st);
}

std::optional<Seen> seen_of(const fs::path& p) {
    struct stat st {};
    if (::stat(p.c_str(), &st) != 0) return std::nullopt;
    return seen_of(st);
}
#endif

} // namespace

LocalFile::LocalFile(LocalFile&& other) noexcept
    : path_(std::move(other.path_)), size_(other.size_), mtime_ns_(other.mtime_ns_) {
#ifdef _WIN32
    h_ = std::exchange(other.h_, nullptr);
#else
    fd_ = std::exchange(other.fd_, -1);
#endif
}

LocalFile& LocalFile::operator=(LocalFile&& other) noexcept {
    if (this != &other) {
        LocalFile gone(std::move(*this));
        path_ = std::move(other.path_);
        size_ = other.size_;
        mtime_ns_ = other.mtime_ns_;
#ifdef _WIN32
        h_ = std::exchange(other.h_, nullptr);
#else
        fd_ = std::exchange(other.fd_, -1);
#endif
    }
    return *this;
}

LocalFile::~LocalFile() {
#ifdef _WIN32
    if (h_ != nullptr) CloseHandle(h_);
#else
    if (fd_ >= 0) ::close(fd_);
#endif
}

std::expected<LocalFile, std::string> LocalFile::open(const fs::path& p) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) return util::failf("{} is a folder; upload it as a folder", util::path_to_utf8(p));
    LocalFile f;
    f.path_ = p;
#ifdef _WIN32
    constexpr std::size_t max_path = 259; // MAX_PATH, less its NUL
    if (!util::long_paths() && p.native().size() > max_path) {
        return util::failf("{} is too long for Windows here ({} characters, {} at most while long paths are off): turn "
                           "them on (\"Enable Win32 long paths\"), or move it to a shorter folder",
                           util::path_to_utf8(p), p.native().size(), max_path);
    }
    // Shared with every other program: the user's own may go on using it (changed() sees what they did).
    const HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return util::failf("cannot read {}: {}", util::path_to_utf8(p), util::error_message(GetLastError()));
    f.h_ = h;
    if (GetFileType(h) != FILE_TYPE_DISK) return util::failf("{} isn't a file grab can upload", util::path_to_utf8(p));
    const auto seen = seen_of(h);
#else
    f.fd_ = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (f.fd_ < 0) return util::failf("cannot read {}: {}", util::path_to_utf8(p), util::error_message(static_cast<unsigned long>(errno)));
    struct stat st {};
    if (fstat(f.fd_, &st) != 0 || !S_ISREG(st.st_mode)) return util::failf("{} isn't a file grab can upload", util::path_to_utf8(p));
    const auto seen = seen_of(st);
#endif
    if (!seen) return util::failf("cannot read {}: its size is not known", util::path_to_utf8(p));
    f.size_ = seen->stamp.size;
    f.mtime_ns_ = seen->stamp.mtime_ns;
    return f;
}

std::expected<std::size_t, std::string> LocalFile::read_at(std::uint64_t offset, std::span<char> out) const {
    std::size_t done = 0;
    while (done < out.size()) {
        const std::uint64_t at = offset + done;
#ifdef _WIN32
        // At an offset of its own, so streams reading at once don't share a position.
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(at & 0xFFFFFFFFu);
        ov.OffsetHigh = static_cast<DWORD>(at >> 32);
        DWORD got = 0;
        const auto n = static_cast<DWORD>(std::min<std::size_t>(out.size() - done, std::size_t{1} << 30));
        if (!ReadFile(static_cast<HANDLE>(h_), out.data() + done, n, &got, &ov)) {
            if (const DWORD e = GetLastError(); e != ERROR_HANDLE_EOF) {
                return util::failf("cannot read {}: {}", util::path_to_utf8(path_), util::error_message(e));
            }
        }
#else
        const ssize_t got = pread(fd_, out.data() + done, out.size() - done, static_cast<off_t>(at));
        if (got < 0) {
            return util::failf("cannot read {}: {}", util::path_to_utf8(path_), util::error_message(static_cast<unsigned long>(errno)));
        }
#endif
        if (got == 0) break; // the end of the file
        done += static_cast<std::size_t>(got);
    }
    return done;
}

bool LocalFile::changed() const {
    const Stamp then{.size = size_, .mtime_ns = mtime_ns_};
#ifdef _WIN32
    const auto now = seen_of(static_cast<HANDLE>(h_));
#else
    const auto now = seen_of(fd_);
#endif
    if (!now || now->stamp != then) return true;
    // Another file at its name (saved by a program that writes a new file and renames it over the
    // old one); the same file there (a link to it too) or none (moved or deleted meanwhile) leaves
    // what was read as it was.
    const auto at_path = seen_of(path_);
    return at_path && !at_path->same_file(*now) && at_path->stamp != then;
}

// ---- names -------------------------------------------------------------------------------------

std::string remote_child(std::string_view dir, std::string_view name) {
    std::string out(dir);
    if (!out.empty() && !out.ends_with('/')) out += '/';
    out += rclone_enc::from_raw_name(name);
    return out;
}

std::pair<std::string, std::string> split_remote(std::string_view remote_path) {
    const auto slash = remote_path.rfind('/');
    if (slash == std::string_view::npos) return {std::string(), std::string(remote_path)};
    return {std::string(slash == 0 ? remote_path.substr(0, 1) : remote_path.substr(0, slash)),
            std::string(remote_path.substr(slash + 1))};
}

namespace {

constexpr std::string_view temp_suffix = ".grabpart";
constexpr std::size_t name_max = 255; // bytes in a name, as Linux file systems take them

// `raw_name` in the folder `raw_dir` (raw bytes; empty: the login folder).
std::string in_folder(std::string_view raw_dir, std::string_view raw_name) {
    std::string out(raw_dir);
    if (!out.empty() && !out.ends_with('/')) out += '/';
    out += raw_name;
    return out;
}

bool hex6(std::string_view s) {
    return s.size() == 6 && std::ranges::all_of(s, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// The folder as it is shown: grab's form, or "the login folder".
std::string shown_dir(std::string_view dir) { return dir.empty() ? std::string("the login folder") : std::string(dir); }

} // namespace

std::string temp_name(std::string_view raw_name, std::string_view token) {
    std::string name = std::format(".{}.{}{}", raw_name, token, temp_suffix);
    if (name.size() > name_max) name = std::format(".grab-{}{}", token, temp_suffix);
    return name;
}

bool is_temp_name(std::string_view raw_name, std::string_view raw_target, std::string_view token) {
    std::string_view n = raw_name;
    if (!n.starts_with('.') || !n.ends_with(temp_suffix)) return false;
    n.remove_suffix(temp_suffix.size());
    if (n.size() < 8 || !hex6(n.substr(n.size() - 6))) return false;
    if (!token.empty() && n.substr(n.size() - 6) != token) return false;
    n.remove_suffix(6);
    if (n == ".grab-") return true; // the short form, for a name too long for the other
    if (!n.ends_with('.')) return false;
    n.remove_suffix(1);
    n.remove_prefix(1);
    return !n.empty() && (raw_target.empty() || n == raw_target);
}

std::string new_token() {
    std::random_device rd;
    return std::format("{:06x}", rd() & 0xFFFFFF);
}

// ---- what the server offers, and its refusals ----------------------------------------------------

Caps caps_of(sftp::Client& client) {
    Caps caps;
    caps.posix_rename = client.has_extension("posix-rename@openssh.com");
    caps.statvfs = client.has_extension("statvfs@openssh.com");
    if (client.has_extension("limits@openssh.com")) {
        if (const auto l = sftp::limits(client); l && l->write > 0) {
            caps.write = static_cast<std::uint32_t>(std::min<std::uint64_t>(l->write, sftp::max_write));
        }
    }
    return caps;
}

Refusal refusal(sftp::Client& client, const Caps& caps, std::string_view server, std::string_view dir,
                std::string_view raw_dir, const sftp::Status& status, std::uint64_t needed) {
    const std::string where = shown_dir(dir);
    // OpenSSH answers every errno but a few with "Failure" (EROFS, ENOSPC, EDQUOT), and Box.Guest
    // refuses an account that may only read with it too: statvfs tells them apart.
    std::optional<sftp::Statvfs> fs;
    if (caps.statvfs && (status.code == sftp::Code::permission_denied || status.code == sftp::Code::failure)) {
        if (auto v = sftp::statvfs(client, raw_dir.empty() ? std::string_view(".") : raw_dir)) fs = *v;
    }
    const std::string said = util::to_lower(status.message);
    const bool says_read_only = said.find("read-only") != std::string::npos || said.find("read only") != std::string::npos;
    if ((fs && fs->read_only()) || (!fs && status.code == sftp::Code::failure && says_read_only)) {
        return {.why = std::format("{} is read-only for this account: nothing can be uploaded to {}", server, where), .fatal = true};
    }
    if (status.code == sftp::Code::permission_denied ||
        (!fs && status.code == sftp::Code::failure && said.find("denied") != std::string::npos)) {
        return {.why = std::format("{} doesn't let this account write in {}", server, where), .fatal = true};
    }
    if (status.code == sftp::Code::failure && fs && fs->free_bytes() < needed) {
        return {.why = std::format("{}:{} is full: {} free, {} needed", server, where, util::format_size(fs->free_bytes()),
                                   util::format_size(needed)),
                .fatal = true};
    }
    if (status.code == sftp::Code::bad_message) return {.why = "the name is too long for the server", .fatal = true};
    const bool fatal = status.code == sftp::Code::no_such_file || status.code == sftp::Code::op_unsupported;
    return {.why = sftp::describe(status), .fatal = fatal};
}

std::expected<Probe, std::string> probe(ssh::Pool& pool, std::string_view server, std::string_view dir,
                                        std::stop_token stop) {
    std::optional<ssh::Pool::Lease> lease;
    try {
        lease = pool.acquire(stop, ssh::interactive_retry);
        if (!lease) return util::fail("stopped");
        sftp::Client& c = (*lease)->sftp();
        const Caps caps = caps_of(c);
        Probe p;
        p.posix_rename = caps.posix_rename;
        p.max_write = caps.write;
        const std::string raw_dir = rclone_enc::raw_path(dir);
        if (caps.statvfs) {
            if (const auto v = sftp::statvfs(c, raw_dir.empty() ? std::string(".") : raw_dir)) {
                p.free = v->free_bytes();
                if (v->read_only()) {
                    p.why = std::format("{} is read-only for this account: nothing can be uploaded to {}", server, shown_dir(dir));
                    return p;
                }
            }
        }
        const std::string file = in_folder(raw_dir, ".grab-probe-" + new_token());
        auto h = sftp::open_file(c, file, sftp::OpenFlags::write | sftp::OpenFlags::creat | sftp::OpenFlags::excl);
        if (!h) {
            p.why = h.error().code == sftp::Code::no_such_file
                        ? std::format("there is no folder {} on {}", shown_dir(dir), server)
                        : refusal(c, caps, server, dir, raw_dir, h.error(), 1).why;
            return p;
        }
        p.writable = true;
        // A handle that fails to close leaves nothing to undo; the file is removed either way.
        [[maybe_unused]] auto closed = sftp::close_file(c, *h);
        if (auto removed = sftp::remove(c, file); !removed && removed.error().code != sftp::Code::no_such_file) {
            p.left = rclone_enc::from_raw_path(file);
        }
        return p;
    } catch (const ssh::Error& e) {
        return util::fail(e.what());
    } catch (const sftp::Broken& e) {
        if (lease) lease->broken();
        return util::fail(e.what());
    }
}

// ---- records -----------------------------------------------------------------------------------

fs::path record_path(const fs::path& records_dir, std::string_view server, std::string_view remote_path) {
    const std::string key = util::sha256_hex(std::format("{}\n{}", server, remote_path)).substr(0, 16);
    return records_dir / util::path_from_utf8(key + ".json");
}

std::string record_to_json(const Record& r) {
    // A download's State as state_to_json writes it, with the upload's own fields and version.
    auto v = json::parse(fetch::state_to_json(r.state));
    if (!v) return {};
    v->set("version", json::Value::make_number(1));
    v->set("local", json::Value::make_string(r.local));
    // As text: a double holds nanoseconds since 1970 only to about a quarter microsecond.
    v->set("localMtimeNs", json::Value::make_string(std::to_string(r.local_mtime_ns)));
    v->set("temp", json::Value::make_string(r.temp));
    v->set("token", json::Value::make_string(r.token));
    return json::stringify(*v);
}

std::optional<Record> record_from_json(std::string_view text) {
    auto v = json::parse(text);
    if (!v || v->kind != json::Value::Kind::object) return std::nullopt;
    const json::Value* version = v->find("version");
    if (version == nullptr || version->kind != json::Value::Kind::number || version->number != 1) return std::nullopt;
    Record r;
    auto local = v->string_of("local");
    auto mtime = v->string_of("localMtimeNs");
    auto temp = v->string_of("temp");
    auto token = v->string_of("token");
    if (!local || !mtime || !temp || !token || !hex6(*token)) return std::nullopt;
    const auto parsed = std::from_chars(mtime->data(), mtime->data() + mtime->size(), r.local_mtime_ns);
    if (parsed.ec != std::errc() || parsed.ptr != mtime->data() + mtime->size()) return std::nullopt;
    // The ranges as state_from_json checks them (it reads a download's version).
    v->set("version", json::Value::make_number(2));
    auto state = fetch::state_from_json(json::stringify(*v));
    if (!state) return std::nullopt;
    // A temp grab named for this file with this token, and nothing else, is ever removed.
    const std::string target = rclone_enc::raw_path(split_remote(state->path).second);
    if (!is_temp_name(rclone_enc::raw_path(*temp), target, *token)) return std::nullopt;
    r.state = std::move(*state);
    r.local = std::move(*local);
    r.temp = std::move(*temp);
    r.token = std::move(*token);
    return r;
}

namespace {

// The record of `remote_path` on `server` when one parses and is for it.
std::optional<Record> read_record(const fs::path& file, std::string_view server, std::string_view remote_path) {
    constexpr std::uintmax_t record_max = 1 << 20; // a record is a few KiB; no reading anything big
    std::error_code ec;
    if (!fs::is_regular_file(file, ec) || fs::file_size(file, ec) > record_max || ec) return std::nullopt;
    const auto text = util::read_file(file);
    if (!text) return std::nullopt;
    auto r = record_from_json(*text);
    if (!r || r->state.rclone_remote != server || r->state.path != remote_path) return std::nullopt;
    return r;
}

fs::path lock_path(const fs::path& record) {
    fs::path p = record;
    p.replace_extension(".lock");
    return p;
}

// The lock on a record (its own file beside it, as the record is replaced whole at each save),
// while one run holds it; the lock's file goes when the record has gone.
struct Held {
    std::optional<util::FileLock> lock;
    fs::path record;
    fs::path lock_file;
    Held() = default;
    Held(const Held&) = delete;
    Held& operator=(const Held&) = delete;
    ~Held() { release(); }
    void release() noexcept {
        if (!lock) return;
        lock.reset();
        std::error_code ec;
        if (!fs::exists(fs::symlink_status(record, ec))) fs::remove(lock_file, ec);
    }
};

// A path in grab's form: `name` (grab's form too) in the folder `dir`.
std::string joined(std::string_view dir, std::string_view name) {
    if (dir.empty()) return std::string(name);
    return std::format("{}{}{}", dir, dir.ends_with('/') ? "" : "/", name);
}

// ---- a step on a connection --------------------------------------------------------------------

// Why a step failed: refused (the server is full), fatal (trying again can't help) or other, with
// what to tell the user.
struct Trouble {
    budget::Failure kind = budget::Failure::other;
    std::string why;
};

std::unexpected<Trouble> trouble(budget::Failure kind, std::string why) {
    return std::unexpected(Trouble{.kind = kind, .why = std::move(why)});
}

// Runs `step(client)` on a connection of the server's pool. Its seat first, as a download's size
// check takes it: a server that refuses connections is asked again 1, 2, 4, 8, then 15 s apart up
// to fetch::refused_give_up. A reused session the server closed while it sat idle is dropped and,
// with `again` (a step whose first request changes nothing), the step tried once more on another.
template <class T, class Step>
std::expected<T, Trouble> on_connection(const Upload& u, fetch::Connections& connections, const std::stop_token& stop,
                                        bool again, Step&& step) {
    const auto from = Clock::now();
    for (int attempt = 0;; ++attempt) {
        if (!connections.acquire(u.server, u.id, stop, false)) return trouble(budget::Failure::other, "stopped");
        std::expected<T, Trouble> r = trouble(budget::Failure::other, "stopped");
        for (bool retried = false;;) {
            std::optional<ssh::Pool::Lease> lease;
            try {
                lease = u.pool->acquire(stop);
                if (lease) r = step((*lease)->sftp());
            } catch (const ssh::Error& e) {
                r = trouble(e.kind, e.what());
            } catch (const sftp::Unavailable& e) { // the server refuses SFTP: the same on every login
                if (lease) lease->broken();
                r = trouble(budget::Failure::fatal, e.what());
            } catch (const sftp::Broken& e) {
                if (lease) lease->broken();
                if (again && lease && lease->reused() && !std::exchange(retried, true) && !stop.stop_requested()) continue;
                r = trouble(budget::Failure::other, e.what());
            }
            break;
        }
        const bool refused = !r && !stop.stop_requested() && r.error().kind == budget::Failure::refused;
        if (refused) connections.refused(u.server);
        connections.release(u.server, u.id);
        if (r || !refused || stop.stop_requested() || Clock::now() - from >= fetch::refused_give_up) return r;
        std::mutex wait_m;
        std::condition_variable_any wait_cv;
        std::unique_lock wait_lock(wait_m);
        wait_cv.wait_for(wait_lock, stop, std::min(std::chrono::seconds(1 << std::min(attempt, 4)), std::chrono::seconds(15)),
                         [] { return false; });
    }
}

// What is at `raw` on the server: nothing (nullopt), or its attributes, through a link (a link
// that leads nowhere: a file of unknown size and time).
std::expected<std::optional<sftp::Attrs>, sftp::Status> look_at(sftp::Client& c, const std::string& raw) {
    auto a = sftp::lstat(c, raw);
    if (!a) {
        if (a.error().code == sftp::Code::no_such_file) return std::optional<sftp::Attrs>();
        return std::unexpected(a.error());
    }
    if (!a->is_link()) return std::optional<sftp::Attrs>(*a);
    if (auto through = sftp::stat(c, raw)) return std::optional<sftp::Attrs>(*through);
    return std::optional<sftp::Attrs>(sftp::Attrs{.size = {}, .permissions = 0100644, .mtime = {}});
}

std::int64_t seconds_of(std::int64_t ns) {
    return ns >= 0 ? ns / 1'000'000'000 : -((-ns + 999'999'999) / 1'000'000'000);
}

// "2023-05-06T11:18:12.123456789Z": the local file's time in the record.
std::string utc_rfc3339_ns(std::int64_t ns) {
    return std::format("{:%FT%TZ}", std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{ns}});
}

Clash clash_of(const LocalFile& file, std::string_view name, const sftp::Attrs& there) {
    return Clash{.file = std::string(name),
                 .local_size = file.size(),
                 .local_mtime = seconds_of(file.mtime_ns()),
                 .remote_size = there.size.value_or(0),
                 .remote_mtime = there.mtime ? std::optional<std::int64_t>(*there.mtime) : std::nullopt,
                 .remote_is_dir = there.is_dir()};
}

// The first "name (n).ext" in the folder that nothing on the server has (Keep both).
std::expected<std::string, sftp::Status> free_numbered(sftp::Client& c, std::string_view dir, std::string_view name) {
    for (int n = 2; n < 1000; ++n) {
        const std::string candidate = joined(dir, existing::numbered(name, n));
        auto a = sftp::lstat(c, rclone_enc::raw_path(candidate));
        if (!a && a.error().code == sftp::Code::no_such_file) return candidate;
        if (!a) return std::unexpected(a.error());
    }
    return std::unexpected(sftp::Status{.code = sftp::Code::failure, .message = "every numbered name up to 999 is taken"});
}

// What the streams came to.
struct Sent {
    std::string error;    // a failure that ends the upload
    bool changed = false; // the local file got shorter while it was read
    bool gone = false;    // the temp file is no longer on the server
};

// The upload's fixed parts while its ranges are sent.
struct Job {
    const Upload& u;
    fetch::Connections& connections;
    const LocalFile& file;
    Caps caps;
    std::string dir;      // the target's folder, grab's form
    std::string raw_dir;  // and its bytes
    std::string raw_temp; // the temp file's path, raw bytes
    std::string temp;     // and in grab's form, for -v
    fs::path record_file; // empty: no record
};

// Sends what `rec.state` lacks into the temp file: as fetch::run fetches, ranges over as many
// streams as the server's budget lets run, split for idle streams near the end, each stream a
// connection of the pool writing its range with up to write_depth WRITEs and max_unacked bytes in
// flight (Play's depth while it is urgent), its done bytes those the server acknowledged in a row
// from its start. The record is saved every 3 s and at the end.
Sent send_all(const Job& job, Record& rec, const std::function<void(const fetch::Progress&)>& on_progress,
              const std::stop_token& stop) {
    const Upload& u = job.u;
    fetch::Connections& connections = job.connections;
    fetch::State& state = rec.state;
    auto weight = [&] { return u.weight != nullptr ? u.weight->load() : 2; };

    // Shared between the streams, under `m`, per range (the same index as state.ranges, which only
    // grows at the end): being sent, failures in a row, when it may be retried, and how far its
    // stream has sent (a file offset; 0 when nothing past its done bytes), which a split leaves
    // with it.
    std::mutex m;
    std::condition_variable_any wake;
    std::vector<char> busy(state.ranges.size(), 0);
    std::vector<int> tries(state.ranges.size(), 0);
    std::vector<Clock::time_point> not_before(state.ranges.size(), Clock::time_point{});
    std::vector<std::uint64_t> sent_to(state.ranges.size(), 0);
    auto sent_past_done = [&](std::size_t r) -> std::uint64_t {
        const std::uint64_t at = state.ranges[r].start + state.ranges[r].done;
        return busy[r] && sent_to[r] > at ? sent_to[r] - at : 0;
    };
    // While the server refuses this upload's connections and nothing is acknowledged, as a
    // download's (fetch.cpp): one stream at a time asks, 1, 2, 4, 8, then 15 s apart.
    std::optional<Clock::time_point> refused_since;
    int refused_streak = 0;
    Clock::time_point hold_until{};
    Sent out;
    std::stop_source inner; // ends all streams: a stop from outside, or a failure that ends the upload
    std::stop_callback forward(stop, [&] { inner.request_stop(); });
    const std::stop_token token = inner.get_token();

    auto next_range = [&](std::optional<Clock::time_point>& retry_at) -> std::optional<std::size_t> {
        const auto now = Clock::now();
        if (refused_since) {
            if (now < hold_until) {
                retry_at = hold_until;
                return std::nullopt;
            }
            if (std::ranges::find(busy, 1) != busy.end()) {
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
        auto open = [&](std::size_t r) { return state.ranges[r].remaining() - sent_past_done(r); };
        std::optional<std::size_t> busiest;
        for (std::size_t r = 0; r < state.ranges.size(); ++r) {
            if (busy[r] && (!busiest || open(r) > open(*busiest))) busiest = r;
        }
        if (!busiest) return std::nullopt;
        auto tail = fetch::split(state, *busiest, sent_past_done(*busiest), fetch::min_split_grab);
        if (tail) {
            busy.push_back(0);
            tries.push_back(0);
            not_before.emplace_back();
            sent_to.push_back(0);
        }
        return tail;
    };

    std::atomic<int> holding{0};        // connections this file's streams hold (Upload::streams_cap)
    std::atomic<std::uint64_t> acked{0}; // bytes the server acknowledged in this run: its speed

    struct Attempt {
        std::uint64_t got = 0;      // bytes of the range acknowledged in a row
        bool worked = false;        // counted as working in the pool, from its first acknowledgement
        bool yielded = false;
        bool gave_way = false;
        bool cut = false;
        bool changed = false;       // the local file got shorter
        bool gone = false;          // the temp is gone on the server
        std::string local_error;    // the local file couldn't be read
        std::optional<budget::Failure> failure;
        std::string error;
    };
    // Range `i` acknowledged up to the file offset `to`. False: stop sending, the range is done
    // (a split may have shortened it meanwhile).
    auto advance = [&](std::size_t i, std::uint64_t to, Attempt& a) {
        std::lock_guard lock(m);
        fetch::Range& r = state.ranges[i];
        const std::uint64_t done = std::min(to - r.start, r.length);
        if (done > r.done) {
            a.got += done - r.done;
            r.done = done;
            refused_since.reset();
            refused_streak = 0;
            if (!a.worked) {
                a.worked = true;
                connections.working(u.server, u.id);
            }
        }
        return r.remaining() > 0;
    };
    auto step_aside = [&](Attempt& a) {
        if (connections.yield_if_over_share(u.server, u.id)) {
            a.yielded = true;
            return true;
        }
        if (u.streams_cap) {
            const int cap = u.streams_cap();
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
    // The bytes the file still needs on the server: what a full disk is told it lacks.
    auto still_needed = [&] {
        std::lock_guard lock(m);
        return state.size - state.bytes_done();
    };

    // Range `i` from `offset` (its first byte not acknowledged) on a connection of the pool: the
    // temp opened for writing (not created, not emptied), WRITEs of what is read from the local
    // file at the same offsets, each sent as it is queued and the replies already here taken
    // between them (sftp::write_range's way), waiting for one only at a cap. Under a speed limit,
    // WRITEs of an eighth of a second of its part, each sent right after its tokens, at most
    // fetch::limited_in_flight on their way; a limit switched on or lowered below what it has on
    // its way (fetch::limited_keep) drops the connection (`a.cut`), else charges that to the new
    // rate, as downloads do.
    auto send_range = [&](std::size_t i, std::uint64_t offset, Attempt& a) {
        std::optional<ssh::Pool::Lease> lease;
        try {
            lease = u.pool->acquire(token);
        } catch (const ssh::Error& e) {
            a.failure = e.kind;
            a.error = e.what();
            return;
        }
        if (!lease) return; // stopped
        try {
            sftp::Client& c = (*lease)->sftp();
            auto handle = sftp::open_file(c, job.raw_temp, sftp::OpenFlags::write);
            if (!handle) {
                if (handle.error().code == sftp::Code::no_such_file) {
                    a.gone = true;
                    a.failure = budget::Failure::fatal;
                    a.error = std::format("the partial upload {} is gone from {}", job.temp, u.server);
                    return;
                }
                const Refusal no = refusal(c, job.caps, u.server, job.dir, job.raw_dir, handle.error(), still_needed());
                a.failure = no.fatal ? budget::Failure::fatal : budget::Failure::other;
                a.error = no.why;
                return;
            }
            std::uint64_t next = offset;   // the next byte to send
            std::uint64_t in_row = offset; // every byte before it acknowledged
            std::map<std::uint64_t, std::uint32_t> early; // acknowledged past in_row
            int in_flight = 0;
            std::uint64_t bytes_in_flight = 0;
            bool sending = true; // false: no more WRITEs (the range is done, the stream steps aside, a failure)
            bool stepped_aside = false;
            std::optional<sftp::Status> write_failed;
            std::vector<char> buffer(job.caps.write);
            auto acknowledged = [&](std::uint64_t at, std::uint32_t n) {
                acked += n;
                if (u.on_sent) u.on_sent(n);
                if (at != in_row) {
                    early.emplace(at, n);
                    return;
                }
                in_row += n;
                for (auto it = early.begin(); it != early.end() && it->first == in_row; it = early.erase(it)) in_row += it->second;
                if (!advance(i, in_row, a)) sending = false;
                else if (sending && step_aside(a)) {
                    sending = false;
                    stepped_aside = true;
                }
            };
            // One WRITE of [at, at + n) as read now: on its way at once, the replies already here
            // taken. False: the local file couldn't give those bytes.
            auto send = [&](std::uint64_t at, std::uint32_t n) {
                const auto got = job.file.read_at(at, std::span(buffer.data(), n));
                if (!got) {
                    a.local_error = got.error();
                    return false;
                }
                if (*got < n) {
                    a.changed = true;
                    return false;
                }
                ++in_flight;
                bytes_in_flight += n;
                c.write(*handle, at, std::string_view(buffer.data(), n), [&, at, n](std::expected<void, sftp::Status> r) {
                    --in_flight;
                    bytes_in_flight -= n;
                    if (!r) {
                        if (!write_failed) write_failed = std::move(r.error());
                        sending = false;
                        return;
                    }
                    acknowledged(at, n);
                });
                c.flush();
                c.pump_ready();
                return true;
            };
            // The next WRITE's length from `next`, up to `size` and the range's end as it is now (0:
            // nothing left to send), its bytes this stream's from then on (sent_to).
            auto next_length = [&](std::uint64_t size) -> std::uint64_t {
                std::lock_guard lock(m);
                const fetch::Range& r = state.ranges[i];
                const std::uint64_t end = r.start + r.length;
                if (next >= end) return 0;
                const std::uint64_t n = std::min(size, end - next);
                sent_to[i] = next + n;
                return n;
            };
            rate::RateLimiter::Setting seen = u.limiter != nullptr ? u.limiter->setting() : rate::RateLimiter::Setting{};
            rate::Place place(u.limiter);
            for (;;) {
                if (u.limiter != nullptr) {
                    if (const auto now = u.limiter->setting(); now.generation != seen.generation) {
                        place.drop(); // asked for at the old rate
                        const bool tighter = now.rate > 0 && (seen.rate == 0 || now.rate < seen.rate);
                        seen = now;
                        if (tighter && bytes_in_flight > fetch::limited_keep(seen.rate, holding.load())) {
                            a.cut = true;
                            sending = false;
                            break;
                        }
                        if (tighter && bytes_in_flight > 0) u.limiter->debit(bytes_in_flight);
                    }
                }
                // While Play on this server warms up or waits for bytes, its depth (urgent).
                const int depth = connections.read_depth(u.server, write_depth);
                if (seen.rate == 0) {
                    while (sending && in_flight < depth && !token.stop_requested()) {
                        const std::uint64_t n = next_length(job.caps.write);
                        if (n == 0) {
                            sending = false;
                            break;
                        }
                        if (in_flight > 0 && bytes_in_flight + n > sftp::max_unacked) break;
                        if (!send(next, static_cast<std::uint32_t>(n))) {
                            sending = false;
                            break;
                        }
                        next += n;
                    }
                } else {
                    const int streams = holding.load();
                    const std::uint64_t most = fetch::limited_in_flight(seen.rate, streams);
                    const std::uint64_t size = std::min<std::uint64_t>(fetch::limited_read(seen.rate, streams), job.caps.write);
                    while (sending && in_flight < depth && !token.stop_requested()) {
                        if (place.ticket == 0) {
                            const std::uint64_t n = next_length(size);
                            if (n == 0) {
                                sending = false;
                                break;
                            }
                            if (in_flight > 0 && bytes_in_flight + n > most) break;
                            place.n = n;
                            place.ticket = u.limiter->enqueue(n, rate::flow_of(u.id, u.member), static_cast<double>(weight()),
                                                              u.flow_part ? u.flow_part() : 0);
                            if (place.ticket == 0) { // unlimited since: seen at the next look
                                if (!send(next, static_cast<std::uint32_t>(n))) {
                                    sending = false;
                                    break;
                                }
                                next += n;
                                continue;
                            }
                        }
                        if (!u.limiter->ready(place.ticket)) {
                            if (in_flight > 0) break;
                            if (!u.limiter->wait(place.ticket, token)) { // a new rate, or a stop: given up
                                place.ticket = 0;
                                break;
                            }
                        }
                        place.ticket = 0;
                        if (!send(next, static_cast<std::uint32_t>(place.n))) {
                            sending = false;
                            break;
                        }
                        next += place.n;
                    }
                }
                if (token.stop_requested() || (in_flight == 0 && !sending) ||
                    (stepped_aside && bytes_in_flight > fetch::hand_over_max))
                    break;
                if (in_flight > 0) c.pump();
                // else: waiting for its tokens ended with a new rate, looked at on the next pass
            }
            place.drop();
            if (token.stop_requested() || in_flight > 0) {
                lease->broken(); // its replies still on the way: closed, not waited for
                return;
            }
            if (write_failed) {
                const Refusal no = refusal(c, job.caps, u.server, job.dir, job.raw_dir, *write_failed, still_needed());
                a.failure = no.fatal ? budget::Failure::fatal : budget::Failure::other;
                a.error = no.why;
            }
            // A handle that won't close leaves nothing that matters here: the bytes were acknowledged.
            [[maybe_unused]] auto closed = sftp::close_file(c, *handle);
        } catch (const sftp::Unavailable& e) { // the server has no SFTP: no use trying again
            a.failure = budget::Failure::fatal;
            a.error = e.what();
        } catch (const sftp::Broken& e) {
            lease->broken();
            a.failure = budget::Failure::other;
            a.error = e.what();
        }
    };

    auto stream = [&] {
        // A stream dropped for a new limit (Attempt::cut) still counts as sending while it logs in
        // again (Upload::on_reading), until its next range starts, or it waits or ends.
        bool carried = false;
        auto uncarry = [&] {
            if (std::exchange(carried, false) && u.on_reading) u.on_reading(-1, false);
        };
        struct Uncarry {
            decltype(uncarry)& f;
            ~Uncarry() { f(); }
        } uncarry_at_end{uncarry};
        for (;;) {
            int h = holding.load();
            const int cap = u.streams_cap ? u.streams_cap() : std::numeric_limits<int>::max();
            while (h < cap && !holding.compare_exchange_weak(h, h + 1)) {
            }
            if (h >= cap) {
                uncarry();
                std::unique_lock lock(m);
                wake.wait_until(lock, token, Clock::now() + std::chrono::milliseconds(250), [] { return false; });
                if (token.stop_requested()) return;
                continue;
            }
            if (!connections.acquire(u.server, u.id, token, false)) {
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
                    uncarry();
                    connections.release(u.server, u.id);
                    --holding;
                    if (!retry_at || token.stop_requested()) return;
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
            if (u.on_command) {
                u.on_command({"sftp", "write", job.temp, "--offset", std::to_string(offset), "--count", std::to_string(count)});
            }
            {
                struct Sending {
                    const Upload& u;
                    bool& carried;
                    bool all_in = false;
                    bool carry = false;
                    Sending(const Upload& up, bool& c) : u(up), carried(c) {
                        if (!std::exchange(carried, false) && u.on_reading) u.on_reading(+1, false);
                    }
                    ~Sending() {
                        if (carry) carried = true;
                        else if (u.on_reading) u.on_reading(-1, all_in);
                    }
                    Sending(const Sending&) = delete;
                    Sending& operator=(const Sending&) = delete;
                } sending(u, carried);
                send_range(i, offset, a);
                sending.carry = a.cut && !token.stop_requested();
                if (u.on_reading) {
                    std::lock_guard lock(m);
                    sending.all_in = state.complete();
                }
            }
            const bool refused = a.failure == budget::Failure::refused && !token.stop_requested();
            if (refused) connections.refused(u.server);
            if (!a.yielded) connections.release(u.server, u.id, 1, a.worked);
            if (!a.gave_way) --holding;

            std::lock_guard lock(m);
            busy[i] = 0;
            sent_to[i] = 0;
            wake.notify_all();
            if (token.stop_requested()) return;
            if (a.changed || a.gone || !a.local_error.empty()) {
                out.changed = out.changed || a.changed;
                out.gone = out.gone || a.gone;
                if (out.error.empty()) out.error = !a.local_error.empty() ? a.local_error : a.error;
                inner.request_stop();
                return;
            }
            if (a.got > 0) tries[i] = 0;
            if (a.yielded || a.gave_way || a.cut) continue;
            if (a.failure == budget::Failure::fatal) {
                if (out.error.empty()) out.error = a.error;
                inner.request_stop();
                return;
            }
            if (state.ranges[i].remaining() == 0) continue;
            if (refused) {
                const auto now = Clock::now();
                if (!refused_since) refused_since = now;
                if (now - *refused_since >= fetch::refused_give_up) {
                    if (out.error.empty()) out.error = "the server keeps refusing connections: " + a.error;
                    inner.request_stop();
                    return;
                }
                hold_until = now + std::min(std::chrono::seconds(1 << std::min(refused_streak++, 4)), std::chrono::seconds(15));
                not_before[i] = now + std::chrono::seconds(1);
                continue;
            }
            if (++tries[i] > fetch::max_retries) {
                if (out.error.empty()) out.error = a.error.empty() ? "the server stopped taking the upload" : a.error;
                inner.request_stop();
                return;
            }
            not_before[i] = Clock::now() + transfer::backoff(tries[i]);
        }
    };

    const std::size_t stream_count =
        state.complete() ? 0 : std::clamp<std::size_t>(static_cast<std::size_t>(connections.budget(u.server)), 1, 16);
    std::atomic<std::size_t> running{stream_count};
    std::vector<std::jthread> streams;
    streams.reserve(stream_count);
    for (std::size_t s = 0; s < stream_count; ++s) {
        streams.emplace_back([&] {
            stream();
            --running;
        });
    }

    // Progress, and the record saved, until the streams are done; the speed as a download's
    // (fetch.cpp): what the server acknowledged over about the last 5 s, started again at a new limit.
    speed::Meter meter;
    meter.sample(Clock::now(), 0);
    std::uint64_t limit_seen = u.limiter != nullptr ? u.limiter->generation() : 0;
    auto last_save = Clock::now();
    auto checkpoint = [&] {
        if (job.record_file.empty()) return;
        Record snapshot;
        {
            std::lock_guard lock(m);
            snapshot = rec;
        }
        // Progress that can't be saved would be lost at the next stop: said, not carried on as if
        // it were.
        if (auto saved = util::write_file_atomic(job.record_file, record_to_json(snapshot)); !saved) {
            std::lock_guard lock(m);
            if (out.error.empty()) out.error = std::format("the progress can't be saved: {}", saved.error());
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
        if (u.limiter != nullptr) {
            if (const auto g = u.limiter->generation(); g != limit_seen) {
                limit_seen = g;
                meter.restart(now, acked.load());
            }
        }
        meter.sample(now, acked.load());
        fetch::Progress p;
        p.bytes = bytes;
        p.total = state.size;
        p.speed = meter.rate();
        if (p.speed > 0 && p.total > p.bytes) p.eta = static_cast<double>(p.total - p.bytes) / p.speed;
        p.connections = connections.active(u.server, u.id);
        if (on_progress) on_progress(p);
        if (now - last_save >= std::chrono::seconds(3)) {
            checkpoint();
            last_save = now;
        }
    }
    streams.clear(); // join
    checkpoint();
    return out;
}

} // namespace

// ---- an upload ---------------------------------------------------------------------------------

Result run(const Upload& u, fetch::Connections& connections, const std::function<void(const fetch::Progress&)>& on_progress,
           std::stop_token stop) {
    auto failed = [](std::string why, std::string note = {}) {
        Result r;
        r.error = std::move(why);
        r.note = std::move(note);
        return r;
    };
    if (!u.pool) return failed("no connection to the server was set up for this upload");
    const auto split = split_remote(u.remote_path);
    const std::string& dir = split.first;
    const std::string& name = split.second;
    if (name.empty() || name == "." || name == "..") return failed("the upload has no name on the server to go under");
    auto opened = LocalFile::open(u.local);
    if (!opened) return failed(opened.error());
    LocalFile file = std::move(*opened);
    const std::string raw_dir = rclone_enc::raw_path(dir);
    const std::string raw_name = rclone_enc::raw_path(name);
    const std::string raw_target = rclone_enc::raw_path(u.remote_path);
    auto weight = [&] { return u.weight != nullptr ? u.weight->load() : 2; };
    // The upload's place in its server's pool, from its first look on.
    transfer::Seat seat(connections, u.server, u.id, u.joined, weight());

    Result result;
    std::string note;
    auto stopped = [&] {
        Result r;
        r.outcome = Outcome::stopped;
        r.note = note;
        r.size = file.size();
        return r;
    };
    for (int round = 0;; ++round) {
        result.size = file.size();
        // A file this big keeps a record, held by this run alone: the window and the command can't
        // upload to the same place at once.
        const bool recorded = !u.records_dir.empty() && file.size() >= small_limit;
        const fs::path record_file = recorded ? record_path(u.records_dir, u.server, u.remote_path) : fs::path();
        Held held;
        if (recorded) {
            auto lock = util::lock_file(lock_path(record_file), std::chrono::milliseconds(0));
            if (!lock) {
                if (lock.error().ends_with("stayed locked")) {
                    return failed(std::format("{}:{} is being uploaded already, by another grab", u.server, u.remote_path), note);
                }
                return failed(std::format("the progress can't be saved: {}", lock.error()), note);
            }
            held.lock = std::move(*lock);
            held.record = record_file;
            held.lock_file = lock_path(record_file);
        }
        const auto old = recorded ? read_record(record_file, u.server, u.remote_path) : std::nullopt;
        const std::string raw_old_temp = old ? in_folder(raw_dir, rclone_enc::raw_path(old->temp)) : std::string();

        // The first look: what the server offers, what is at the target's name, and the temp the
        // record names.
        struct Start {
            Caps caps;
            std::optional<sftp::Attrs> target;
            std::optional<sftp::Attrs> temp;
        };
        if (u.on_command) u.on_command({"sftp", "stat", u.remote_path});
        auto start = on_connection<Start>(u, connections, stop, true, [&](sftp::Client& c) -> std::expected<Start, Trouble> {
            Start s;
            s.caps = caps_of(c);
            auto there = look_at(c, raw_target);
            if (!there) {
                return trouble(budget::Failure::fatal,
                               refusal(c, s.caps, u.server, dir, raw_dir, there.error(), file.size()).why);
            }
            s.target = *there;
            if (old) {
                if (auto t = sftp::lstat(c, raw_old_temp)) s.temp = *t;
            }
            return s;
        });
        if (stop.stop_requested()) return stopped();
        if (!start) return failed(start.error().why, note);
        const Caps caps = start->caps;

        // A file there: the choice decides, before anything is sent.
        if (start->target) {
            const Clash clash = clash_of(file, name, *start->target);
            const Decision d = decide(u.existing, clash);
            if (d.action == Action::ask || d.action == Action::skip) {
                result.clash = clash;
                result.note = d.note.empty() ? note : d.note;
                result.outcome = d.action == Action::ask ? Outcome::exists : Outcome::skipped;
                if (d.action == Action::skip && old) { // a partial upload of it is no longer wanted
                    held.release();
                    if (auto gone = discard(*u.pool, u.records_dir, u.server, u.remote_path, stop); !gone) {
                        result.temp = rclone_enc::from_raw_path(raw_old_temp);
                    }
                }
                return result;
            }
        }

        // Going on from the record: the same local file (size, time), and the temp it names there,
        // a file, holding at least every byte the record counts and no more than the file. Each
        // acknowledged byte was written at its offset; a range not begun (done 0) proves nothing.
        std::uint64_t counted_to = 0;
        if (old) {
            for (const auto& r : old->state.ranges)
                if (r.done > 0) counted_to = std::max(counted_to, r.start + r.done);
        }
        const bool resume = old && start->temp && start->temp->is_file() && old->state.size == file.size() &&
                            old->local_mtime_ns == file.mtime_ns() && start->temp->size &&
                            *start->temp->size >= counted_to && *start->temp->size <= file.size();
        Record rec;
        if (resume) {
            rec = *old;
        } else {
            if (old && note.empty()) note = "the file changed since its partial upload, or that was damaged; started over";
            const int expected_streams = u.streams_cap ? u.streams_cap() : connections.budget(u.server);
            rec.state = fetch::new_state(u.server, u.remote_path, file.size(), utc_rfc3339_ns(file.mtime_ns()),
                                         fetch::chunk_size_for(file.size(), expected_streams, fetch::min_split_grab));
            rec.local = util::path_to_utf8(file.path());
            rec.local_mtime_ns = file.mtime_ns();
            // The old temp goes (grab's: its record's token proves it), then a new one is made,
            // exclusively, under a new token, the record written first so the temp never lacks one.
            // `creating`: the temp an OPEN was last sent for; a link that broke before its answer
            // leaves it made, which the step, tried again on another login, finds (empty) and keeps.
            std::string creating;
            auto made = on_connection<bool>(u, connections, stop, true, [&](sftp::Client& c) -> std::expected<bool, Trouble> {
                if (!creating.empty()) {
                    if (auto a = sftp::lstat(c, creating); a && a->is_file() && a->size.value_or(1) == 0) return true;
                    creating.clear();
                }
                if (start->temp) {
                    if (u.on_command) u.on_command({"sftp", "rm", rclone_enc::from_raw_path(raw_old_temp)});
                    if (auto removed = sftp::remove(c, raw_old_temp); !removed && removed.error().code != sftp::Code::no_such_file) {
                        return trouble(budget::Failure::fatal, refusal(c, caps, u.server, dir, raw_dir, removed.error(), 0).why);
                    }
                }
                for (int attempt = 0; attempt < 3; ++attempt) {
                    rec.token = new_token();
                    rec.temp = rclone_enc::from_raw_name(temp_name(raw_name, rec.token));
                    const std::string raw_temp = in_folder(raw_dir, rclone_enc::raw_path(rec.temp));
                    if (recorded) {
                        if (auto saved = util::write_file_atomic(record_file, record_to_json(rec)); !saved) {
                            return trouble(budget::Failure::fatal, std::format("the progress can't be saved: {}", saved.error()));
                        }
                    }
                    if (u.on_command) u.on_command({"sftp", "create", rclone_enc::from_raw_path(raw_temp)});
                    creating = raw_temp;
                    auto h = sftp::open_file(c, raw_temp, sftp::OpenFlags::write | sftp::OpenFlags::creat | sftp::OpenFlags::excl);
                    if (h) {
                        [[maybe_unused]] auto closed = sftp::close_file(c, *h);
                        return true;
                    }
                    creating.clear();
                    std::error_code ec;
                    if (recorded) fs::remove(record_file, ec);
                    if (h.error().code == sftp::Code::no_such_file) {
                        return trouble(budget::Failure::fatal, std::format("there is no folder {} on {}", shown_dir(dir), u.server));
                    }
                    if (h.error().code == sftp::Code::failure && sftp::lstat(c, raw_temp)) continue; // taken: another token
                    return trouble(budget::Failure::fatal, refusal(c, caps, u.server, dir, raw_dir, h.error(), file.size()).why);
                }
                return trouble(budget::Failure::other, "no free name for the upload's temp file");
            });
            if (stop.stop_requested()) {
                // A temp made as the stop came: without a record, only the caller can remove it.
                Result r = stopped();
                if (made && !recorded) r.temp = joined(dir, rec.temp);
                return r;
            }
            if (!made) return failed(made.error().why, note);
        }
        const Job job{.u = u,
                      .connections = connections,
                      .file = file,
                      .caps = caps,
                      .dir = dir,
                      .raw_dir = raw_dir,
                      .raw_temp = in_folder(raw_dir, rclone_enc::raw_path(rec.temp)),
                      .temp = joined(dir, rec.temp),
                      .record_file = record_file};
        // What a stopped or failed upload leaves on the server for the caller, without a record.
        const std::string left = recorded ? std::string() : job.temp;
        auto ended = [&](Outcome outcome, std::string error) {
            Result r;
            r.outcome = outcome;
            r.error = std::move(error);
            r.note = note;
            r.size = file.size();
            r.temp = left;
            return r;
        };

        const Sent sent = send_all(job, rec, on_progress, stop);
        if (stop.stop_requested()) return ended(Outcome::stopped, {});
        // Started over once when the local file changed, or the server lost the temp or its bytes.
        bool changed = sent.changed;
        bool wrong_size = false;
        if (!sent.changed && !sent.gone && sent.error.empty() && rec.state.complete()) {
            // Complete: the server's copy as long as the file, which hasn't changed meanwhile.
            if (u.on_command) u.on_command({"sftp", "stat", job.temp});
            auto size = on_connection<std::uint64_t>(u, connections, stop, true, [&](sftp::Client& c) -> std::expected<std::uint64_t, Trouble> {
                auto a = sftp::stat(c, job.raw_temp);
                if (!a) {
                    if (a.error().code == sftp::Code::no_such_file) return std::numeric_limits<std::uint64_t>::max();
                    return trouble(budget::Failure::other, sftp::describe(a.error()));
                }
                return a->size.value_or(0);
            });
            if (stop.stop_requested()) return ended(Outcome::stopped, {});
            if (!size) return ended(Outcome::failed, size.error().why);
            changed = file.changed();
            wrong_size = *size != file.size();
        }
        if (sent.gone || changed || wrong_size) {
            const std::string why = sent.gone ? "the partial upload was gone from the server"
                                    : changed ? std::format("{} changed while uploading", util::path_to_utf8(u.local))
                                              : "the server's copy came out the wrong size";
            // The temp goes (ours, by its token), and the record: nothing of them is any use.
            auto removed = on_connection<bool>(u, connections, stop, true, [&](sftp::Client& c) -> std::expected<bool, Trouble> {
                if (auto r = sftp::remove(c, job.raw_temp); !r && r.error().code != sftp::Code::no_such_file) {
                    return trouble(budget::Failure::other, sftp::describe(r.error()));
                }
                return true;
            });
            std::error_code ec;
            if (removed && recorded) fs::remove(record_file, ec);
            if (stop.stop_requested()) return ended(Outcome::stopped, {});
            if (round > 0) {
                Result r = ended(Outcome::failed, why);
                if (!removed) r.temp = job.temp;
                return r;
            }
            note = why + "; started over";
            held.release();
            auto reopened = LocalFile::open(u.local);
            if (!reopened) return failed(reopened.error(), note);
            file = std::move(*reopened);
            continue;
        }
        if (!sent.error.empty()) return ended(Outcome::failed, sent.error);
        if (!rec.state.complete()) return ended(Outcome::failed, "the upload ended incomplete");

        // In place: the file's time first, then its name by the rules (UPLOADS.md 5.3), the name
        // looked at right before each try.
        struct Placed {
            Outcome outcome = Outcome::done;
            std::string placed;
            std::optional<Clash> clash;
            std::string note;
        };
        const auto mtime = static_cast<std::uint32_t>(std::clamp<std::int64_t>(seconds_of(file.mtime_ns()), 0, std::numeric_limits<std::uint32_t>::max()));
        // `renaming`: the name a rename was last sent for. The step runs again on another login when
        // the link broke, perhaps after the server had renamed the temp: then the file is found there.
        std::string renaming;
        // The server refused to set the time (some gateways do): the file goes in place all the same.
        std::string time_kept;
        auto placed = on_connection<Placed>(u, connections, stop, true, [&](sftp::Client& c) -> std::expected<Placed, Trouble> {
            if (auto timed = sftp::setstat(c, job.raw_temp, sftp::Attrs{.size = {}, .permissions = {}, .mtime = mtime}); !timed) {
                switch (timed.error().code) {
                case sftp::Code::no_such_file: {
                    if (!renaming.empty()) {
                        const auto a = sftp::lstat(c, rclone_enc::raw_path(renaming));
                        if (a && a->is_file() && a->size == file.size() && (!time_kept.empty() || a->mtime == mtime)) {
                            return Placed{.outcome = Outcome::done, .placed = renaming, .clash = {}, .note = {}};
                        }
                    }
                    return trouble(budget::Failure::fatal, "the partial upload was gone from the server");
                }
                case sftp::Code::op_unsupported:
                case sftp::Code::permission_denied:
                case sftp::Code::failure:
                    time_kept = "the server kept its own time for it; Replace if newer / if different will see it as changed";
                    break;
                default: return trouble(budget::Failure::other, refusal(c, caps, u.server, dir, raw_dir, timed.error(), 0).why);
                }
            }
            std::string target = u.remote_path;
            for (int tries = 0; tries < 8; ++tries) {
                const std::string raw = rclone_enc::raw_path(target);
                auto there = look_at(c, raw);
                if (!there) return trouble(budget::Failure::fatal, refusal(c, caps, u.server, dir, raw_dir, there.error(), 0).why);
                if (!*there) {
                    // A new name: RENAME, which never replaces anything on OpenSSH; a name taken
                    // meanwhile gets the choice on the next round.
                    if (u.on_command) u.on_command({"sftp", "rename", job.temp, target});
                    renaming = target;
                    auto moved = sftp::rename(c, job.raw_temp, raw);
                    if (moved) return Placed{.outcome = Outcome::done, .placed = target, .clash = {}, .note = {}};
                    renaming.clear();
                    if (sftp::lstat(c, raw)) continue;
                    return trouble(budget::Failure::fatal, refusal(c, caps, u.server, dir, raw_dir, moved.error(), 0).why);
                }
                const Clash clash = clash_of(file, split_remote(target).second, **there);
                const Decision d = decide(u.existing, clash);
                switch (d.action) {
                case Action::ask: return Placed{.outcome = Outcome::exists, .placed = {}, .clash = clash, .note = {}};
                case Action::skip: {
                    if (u.on_command) u.on_command({"sftp", "rm", job.temp});
                    if (auto r = sftp::remove(c, job.raw_temp); !r && r.error().code != sftp::Code::no_such_file) {
                        return trouble(budget::Failure::other, sftp::describe(r.error()));
                    }
                    return Placed{.outcome = Outcome::skipped, .placed = {}, .clash = clash, .note = d.note};
                }
                case Action::numbered: {
                    auto numbered = free_numbered(c, dir, name);
                    if (!numbered) return trouble(budget::Failure::fatal, refusal(c, caps, u.server, dir, raw_dir, numbered.error(), 0).why);
                    target = *numbered;
                    continue;
                }
                case Action::replace: break;
                }
                if (caps.posix_rename) { // in one step
                    if (u.on_command) u.on_command({"sftp", "posix-rename", job.temp, target});
                    renaming = target;
                    if (auto moved = sftp::posix_rename(c, job.raw_temp, raw); !moved) {
                        return trouble(budget::Failure::fatal, refusal(c, caps, u.server, dir, raw_dir, moved.error(), 0).why);
                    }
                    return Placed{.outcome = Outcome::done, .placed = target, .clash = {}, .note = {}};
                }
                // Without posix-rename: the old file goes first (only because the user chose to
                // replace it), then the new one takes its name.
                if (u.on_command) u.on_command({"sftp", "rm", target});
                if (auto removed = sftp::remove(c, raw); !removed && removed.error().code != sftp::Code::no_such_file) {
                    return trouble(budget::Failure::fatal, refusal(c, caps, u.server, dir, raw_dir, removed.error(), 0).why);
                }
                if (u.on_command) u.on_command({"sftp", "rename", job.temp, target});
                renaming = target;
                if (auto moved = sftp::rename(c, job.raw_temp, raw); !moved) {
                    renaming.clear();
                    if (sftp::lstat(c, raw)) continue; // taken again meanwhile
                    return trouble(budget::Failure::fatal,
                                   std::format("the old {} was removed, but the new file couldn't take its name: it is {} ({})", target,
                                               job.temp, sftp::describe(moved.error())));
                }
                return Placed{.outcome = Outcome::done, .placed = target, .clash = {}, .note = {}};
            }
            return trouble(budget::Failure::other, "the name kept being taken on the server meanwhile");
        });
        if (stop.stop_requested() && !placed) return ended(Outcome::stopped, {});
        if (!placed) return ended(Outcome::failed, placed.error().why);
        result.outcome = placed->outcome;
        result.placed = placed->placed;
        result.clash = placed->clash;
        result.note = placed->note.empty() ? note : placed->note;
        if (result.note.empty() && placed->outcome == Outcome::done) result.note = time_kept;
        if (placed->outcome == Outcome::exists) {
            result.temp = left; // complete: a record goes on from it at once
            return result;
        }
        if (recorded) { // done or skipped: the record goes, and its lock's file with it (Held)
            std::error_code ec;
            fs::remove(record_file, ec);
        }
        if (placed->outcome == Outcome::done && on_progress) {
            on_progress(fetch::Progress{.bytes = file.size(), .total = file.size(), .speed = 0, .eta = std::nullopt, .connections = 0});
        }
        return result;
    }
}

// ---- partial uploads -----------------------------------------------------------------------------

std::optional<std::pair<std::uint64_t, std::uint64_t>> saved_progress(const fs::path& records_dir, std::string_view server,
                                                                      std::string_view remote_path) {
    const auto r = read_record(record_path(records_dir, server, remote_path), server, remote_path);
    if (!r) return std::nullopt;
    return std::pair{r->state.bytes_done(), r->state.size};
}

std::expected<void, std::string> discard(ssh::Pool& pool, const fs::path& records_dir, std::string_view server,
                                         std::string_view remote_path, std::stop_token stop) {
    const fs::path file = record_path(records_dir, server, remote_path);
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(file, ec))) return {};
    auto lock = util::lock_file(lock_path(file), std::chrono::milliseconds(0));
    if (!lock) return util::failf("{}:{} is still being uploaded", server, remote_path);
    const auto r = read_record(file, server, remote_path);
    if (r) {
        const std::string raw = in_folder(rclone_enc::raw_path(split_remote(remote_path).first), rclone_enc::raw_path(r->temp));
        std::optional<ssh::Pool::Lease> lease;
        try {
            lease = pool.acquire(stop, ssh::interactive_retry);
            if (!lease) return util::fail("stopped");
            if (auto removed = sftp::remove((*lease)->sftp(), raw); !removed && removed.error().code != sftp::Code::no_such_file) {
                return util::failf("cannot remove the partial upload {} on {}: {}", rclone_enc::from_raw_path(raw), server,
                                   sftp::describe(removed.error()));
            }
        } catch (const ssh::Error& e) {
            return util::fail(e.what());
        } catch (const sftp::Broken& e) {
            if (lease) lease->broken();
            return util::fail(e.what());
        }
    }
    // A record that names nothing grab can prove its own leaves the server alone.
    fs::remove(file, ec);
    *lock = util::FileLock();
    fs::remove(lock_path(file), ec);
    return {};
}

std::expected<void, std::string> remove_temp(ssh::Pool& pool, std::string_view temp, std::stop_token stop) {
    const std::string raw = rclone_enc::raw_path(temp);
    if (!is_temp_name(rclone_enc::raw_path(split_remote(temp).second), {})) {
        return util::failf("{} isn't a temp file of grab's", temp);
    }
    std::optional<ssh::Pool::Lease> lease;
    try {
        lease = pool.acquire(stop, ssh::interactive_retry);
        if (!lease) return util::fail("stopped");
        if (auto removed = sftp::remove((*lease)->sftp(), raw); !removed && removed.error().code != sftp::Code::no_such_file) {
            return util::failf("cannot remove {}: {}", temp, sftp::describe(removed.error()));
        }
        return {};
    } catch (const ssh::Error& e) {
        return util::fail(e.what());
    } catch (const sftp::Broken& e) {
        if (lease) lease->broken();
        return util::fail(e.what());
    }
}

} // namespace grab::upload
