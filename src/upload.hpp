#pragma once

// grab's own resumable upload of one file (UPLOADS.md 5.2-5.4), a sibling of fetch::run that leaves
// the measured download path as it is. The file is sent in byte ranges: several streams, each on a
// connection of the server's pool (the same budget the downloads share), write their range into
// a temp file beside the target with pipelined SFTP WRITEs, and only a complete file takes the
// target's name. Bytes count once the server acknowledged them; a speed limit charges them as they
// are sent.
//
// The temp file is ".<name>.<token>.grabpart" in the target's folder (".grab-<token>.grabpart"
// when that name gets too long), the token six random hex digits: grab touches no file on the
// server whose name doesn't prove it grab's. A file of small_limit or more has a record of what the
// server acknowledged in records_dir, so stopping keeps the temp and the next run goes on from
// there, also after grab restarts; a smaller one starts over.
//
// A new name is taken with a plain RENAME, which on OpenSSH never replaces anything: a file that
// appeared meanwhile gets the upload's existing-file choice (upload_existing.hpp). Replace uses
// posix-rename@openssh.com where the server offers it (one step), else REMOVE then RENAME.

#include "fetch.hpp"
#include "rate.hpp"
#include "sftp.hpp"
#include "upload_existing.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace grab::ssh {
class Pool;
} // namespace grab::ssh

namespace grab::upload {

// The WRITE size where the server says nothing of its limits (UPLOADS.md section 13); where it
// does (limits@openssh.com), its largest WRITE up to sftp::max_write.
inline constexpr std::uint32_t fallback_write = 32 * 1024;
// WRITEs one connection keeps in flight, and their bytes not yet acknowledged (section 13: 64 and
// 8 MiB; more changed nothing on Main, the connection's TCP flow being the limit).
inline constexpr int write_depth = 64;
// Files below this have no record: they start over (UPLOADS.md 5.4), as a folder's small files
// are fetched whole (folder::small_limit).
inline constexpr std::uint64_t small_limit = std::uint64_t{2} << 20;

// The local file, read at offsets by several streams at once. Opened shared, so the user's own
// programs may still read, write, rename or delete it: a change shows in its size or time
// (changed), and the upload starts over once rather than sending a mix.
class LocalFile {
public:
    LocalFile() = default;
    LocalFile(LocalFile&& other) noexcept;
    LocalFile& operator=(LocalFile&& other) noexcept;
    LocalFile(const LocalFile&) = delete;
    LocalFile& operator=(const LocalFile&) = delete;
    ~LocalFile();

    // A regular file (a folder or anything else is refused with the reason).
    [[nodiscard]] static std::expected<LocalFile, std::string> open(const std::filesystem::path& p);
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }
    [[nodiscard]] std::uint64_t size() const { return size_; }
    [[nodiscard]] std::int64_t mtime_ns() const { return mtime_ns_; } // nanoseconds since 1970
    // `out.size()` bytes from `offset`: fewer only where the file ends. Thread-safe.
    [[nodiscard]] std::expected<std::size_t, std::string> read_at(std::uint64_t offset, std::span<char> out) const;
    // Whether the file was written to since it was opened, or another file took its name: another
    // size or time, through the handle or the path.
    [[nodiscard]] bool changed() const;

private:
    std::filesystem::path path_;
    std::uint64_t size_ = 0;
    std::int64_t mtime_ns_ = 0;
#ifdef _WIN32
    void* h_ = nullptr;
#else
    int fd_ = -1;
#endif
};

// `name` (a local file's or folder's, UTF-8, uploaded as it is) in the server folder `dir`, both in
// grab's form: what rclone_enc::raw_path turns into the folder's bytes, '/', and the name's bytes.
[[nodiscard]] std::string remote_child(std::string_view dir, std::string_view name);
// The server folder a path in grab's form is in ("" for a name in the login folder) and its name.
[[nodiscard]] std::pair<std::string, std::string> split_remote(std::string_view remote_path);
// ".<name>.<token>.grabpart" for a name of raw bytes, or ".grab-<token>.grabpart" when that
// would be longer than a name may be (255 bytes).
[[nodiscard]] std::string temp_name(std::string_view raw_name, std::string_view token);
// Whether `raw_name` is a temp name with `token` (empty: any six hex digits) for the file named
// `raw_target` (empty: any): the only files grab removes on a server.
[[nodiscard]] bool is_temp_name(std::string_view raw_name, std::string_view raw_target = {}, std::string_view token = {});
// Six random hex digits.
[[nodiscard]] std::string new_token();

// How an upload ended. skipped: the choice kept the file there (Result::note says why when it
// wasn't the user's choice); exists: a file is there and the choice is to ask.
enum class Outcome { done, stopped, failed, exists, skipped };

// The bytes this account may still write where the server says (statvfs), its WRITE size and
// whether it replaces in one step: what the connection's server offers.
struct Caps {
    std::uint32_t write = fallback_write;
    bool posix_rename = false;
    bool statvfs = false;
};
// Asked on a session's client (limits@openssh.com when offered).
[[nodiscard]] Caps caps_of(sftp::Client& client);

// What grab tells the user for a change the server refused in the folder `dir` (grab's form; its
// bytes `raw_dir`) on `server`, its statvfs asked (when offered): "<Server> is read-only for this
// account: nothing can be uploaded to <dir>" where it says the file system is read-only (Box.Guest
// refuses with "Failure"), "<Server> doesn't let this account write in <dir>" for "permission
// denied", "<Server>:<dir> is full: X free, Y needed" for a "Failure" with less than `needed`
// bytes free, "the name is too long for the server" for a bad message, else the server's own.
// `fatal`: trying again can't help. `scope`: what else it holds for: the whole server (read-only,
// full), every new file in that folder (no write access, no such folder), or this file alone.
// `kept_temp`: place's "the old X was removed, but the new file couldn't take its name": the temp
// holds the only copy now, and stays.
struct Refusal {
    enum class Scope { file, folder, server };
    std::string why;
    bool fatal = false;
    Scope scope = Scope::file;
    bool kept_temp = false;
};
[[nodiscard]] Refusal refusal(sftp::Client& client, const Caps& caps, std::string_view server, std::string_view dir,
                              std::string_view raw_dir, const sftp::Status& status, std::uint64_t needed);

// The window's write check in a server folder before an upload (UPLOADS.md 5.10): a file of its
// own made exclusively (".grab-probe-<token>"), closed and removed, and what the server offers.
struct Probe {
    bool writable = false;
    std::string why;                   // not writable: refusal's text
    std::optional<std::uint64_t> free; // bytes this account may still write there, where the server says
    bool posix_rename = false;
    std::uint32_t max_write = fallback_write; // the WRITE size grab uses there
    std::string left;                  // the probe file, when it couldn't be removed again (grab's own name)
};
// The connection's failure (a login, a broken link) as the error: whether it can be written is
// then not known.
[[nodiscard]] std::expected<Probe, std::string> probe(ssh::Pool& pool, std::string_view server, std::string_view dir,
                                                      std::stop_token stop = {});

// What is at `raw` on the server: nothing (nullopt), or its attributes, through a link (a link
// that leads nowhere: a file of unknown size and time).
[[nodiscard]] std::expected<std::optional<sftp::Attrs>, sftp::Status> look_at(sftp::Client& c, const std::string& raw);
// The clash of a file of `size` bytes modified at `mtime` (seconds since 1970) with what is at its
// name `name` on the server.
[[nodiscard]] Clash clash_with(std::string_view name, std::uint64_t size, std::int64_t mtime, const sftp::Attrs& there);

// A complete temp file to put in place (place).
struct PlaceRequest {
    std::string server;      // its name in messages
    std::string temp;        // the temp file, grab's form, in the target's folder
    std::string target;      // where it goes, grab's form
    Existing existing = Existing::ask;
    std::uint64_t size = 0;  // the local file's, for the clash
    std::int64_t mtime = 0;  // seconds since 1970
    std::function<void(const std::vector<std::string>&)> on_command;
};
struct Placement {
    Outcome outcome = Outcome::done; // done, exists (the choice is to ask) or skipped
    std::string placed;              // done: where it went (Keep both: the numbered name)
    std::optional<Clash> clash;      // exists, skipped: the file there
    std::string note;                // skipped: why, when it wasn't the user's choice
};
// The temp file into place by the rules (UPLOADS.md 5.3), the name looked at right before each
// try: a new name by a plain RENAME (which never replaces anything on OpenSSH), a taken one by the
// choice (decide): Replace with posix-rename where offered, else REMOVE then RENAME; Skip removes
// the temp; Keep both takes the first numbered name free; Ask leaves the temp (Outcome::exists).
// `renaming` keeps the name a rename was last sent for, so a step tried again after a broken link
// can find the file placed. Blocking requests on `c`; a refusal says why (fatal: trying again
// can't help).
[[nodiscard]] std::expected<Placement, Refusal> place(sftp::Client& c, const Caps& caps, const PlaceRequest& r,
                                                      std::string& renaming);

// What a file's record (records_dir/<hash>.json, record_path) keeps: the State of the file's
// ranges as a download's (fetch::State: rclone_remote = the server, path = the target, modtime =
// the local file's time), the local file and its time, the temp file's name and its token.
struct Record {
    fetch::State state;
    std::string local;           // the local file, UTF-8
    std::int64_t local_mtime_ns = 0;
    std::string temp;            // the temp's name in the target's folder, grab's form
    std::string token;
    bool operator==(const Record&) const = default;
};
// records_dir/<16 hex digits of the SHA-256 of server and path>.json
[[nodiscard]] std::filesystem::path record_path(const std::filesystem::path& records_dir, std::string_view server,
                                                std::string_view remote_path);
[[nodiscard]] std::string record_to_json(const Record& r);
// nullopt for anything malformed, or a temp name without its token.
[[nodiscard]] std::optional<Record> record_from_json(std::string_view text);

struct Upload {
    std::filesystem::path local; // the file on this PC
    std::string server;          // grab.conf section: the connection pool, and its name in messages
    int id = 0;                  // the upload's id in the pools (the same ids as downloads')
    // Where it goes: the file's path on the server in grab's form (remote_child); its folder must
    // be there.
    std::string remote_path;
    // A file already at remote_path (also one that appears meanwhile): what to do with it. ask: the
    // run ends with Outcome::exists, nothing replaced.
    Existing existing = Existing::ask;
    // As fetch::Download's: the caller joined the pool under `id` (a folder's file), the speed
    // limit (uploads' own) and its flow, the priority, the cap on streams, and who is told what.
    bool joined = false;
    rate::RateLimiter* limiter = nullptr;
    const std::atomic<int>* weight = nullptr;
    std::uint32_t member = 0;
    std::function<double()> flow_part;
    std::function<int()> streams_cap;
    // +1 as a stream starts sending on a connection it holds, -1 as it stops, `all_in` once the
    // server acknowledged every byte.
    std::function<void(int delta, bool all_in)> on_reading;
    // The bytes of each WRITE the server acknowledged, as it does: the speeds count those.
    std::function<void(std::uint64_t)> on_sent;
    // Each step before it is taken ("sftp write <path> --offset N --count M"; `grab -v`).
    std::function<void(const std::vector<std::string>&)> on_command;
    std::shared_ptr<ssh::Pool> pool;
    // Where the records of partial uploads are kept (gui_state's state_dir / "uploads"); empty:
    // none, every upload starts over.
    std::filesystem::path records_dir;
};

struct Result {
    Outcome outcome = Outcome::failed;
    std::string error;
    std::string note;   // e.g. "the file changed while uploading; started over"
    std::string placed; // done: where the file went, grab's form (Keep both: the numbered name)
    std::uint64_t size = 0; // the local file's, once looked at
    std::int64_t mtime_ns = 0; // ...and its time (nanoseconds since 1970), which done gave the server's copy
    std::optional<Clash> clash; // exists, skipped: the file there
    // A temp file left on the server without a record (a small file stopped or failed, or one that
    // couldn't be removed), grab's form: the caller removes it (remove_temp) or notes it.
    std::string temp = {};
};

// Uploads `u.local` to `u.remote_path` on `u.pool`'s server, going on from a partial upload's
// record. Stopping keeps the temp file and the record (Outcome::stopped); a failure too, so a
// retry goes on. A file that changes while it is sent starts over once, then fails.
[[nodiscard]] Result run(const Upload& u, fetch::Connections& connections,
                         const std::function<void(const fetch::Progress&)>& on_progress, std::stop_token stop);

// The partial upload of `remote_path` (its record in `records_dir`): bytes acknowledged and the
// file's size; nullopt when there is none.
[[nodiscard]] std::optional<std::pair<std::uint64_t, std::uint64_t>>
saved_progress(const std::filesystem::path& records_dir, std::string_view server, std::string_view remote_path);
// Cancel: removes the partial upload of `remote_path`, its temp file on the server (only a name
// with the record's token, in the target's folder) and then its record. An error when the server
// can't be reached or refuses: the record stays, so it can be tried again.
[[nodiscard]] std::expected<void, std::string> discard(ssh::Pool& pool, const std::filesystem::path& records_dir,
                                                       std::string_view server, std::string_view remote_path,
                                                       std::stop_token stop = {});
// Removes a temp file left on the server (Result::temp): refused unless its name is a temp name.
// A temp that is gone already is no error.
[[nodiscard]] std::expected<void, std::string> remove_temp(ssh::Pool& pool, std::string_view temp,
                                                           std::stop_token stop = {});

} // namespace grab::upload
