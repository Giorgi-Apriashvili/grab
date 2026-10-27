#pragma once

// Checking downloads against the server's copy (optional: Settings → Downloads, `grab --verify`),
// on every server through grab's own SSH client (RCLONE-REPLACEMENT.md, phase 5): the server's
// md5sum over the pool's connections (ssh_hash.hpp) and grab's MD5 of the local copy, both at the
// same time once the download is done, each file compared by its own path, whatever its local name
// (Keep both, a device name, a case clash, a name that isn't UTF-8).
//
// Only a proven mismatch costs a copy: the sums differ, one SFTP STAT shows the server's file as
// the download left it, and the copy's size and time are what the download set. Anything else
// keeps the copy as "not verified: <why>": no login, a refused command, no md5sum, a name the
// server's shell can't be given, a server that took too long, a stop, or either copy changed since
// the download.

#include "engine.hpp"
#include "fetch.hpp"
#include "ssh_pool.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace grab::verify {

// One downloaded file to check.
struct File {
    std::string path;            // on the server, relative to Request::remote_dir (grab's form)
    std::filesystem::path local; // the copy
    std::uint64_t size = 0;      // the server's file as the download fetched it
    std::string modtime;         // ...and its time (RFC 3339); "" when not known
};

enum class Verdict {
    same,      // the sums agree
    differs,   // proven: the sums differ and neither copy changed since the download (delete it)
    missing,   // the copy is gone (nothing to delete; downloading it again brings it back)
    unchecked, // not verified: `why`
};
struct Result {
    Verdict verdict = Verdict::unchecked;
    std::string why; // unchecked: in a few words ("the server's copy changed since the download")
    bool operator==(const Result&) const = default;
};

struct Request {
    const engine::Context* ctx = nullptr; // the server: rclone.conf's keys for it (ssh::hash_blocked)
    // grab's own connections to the server, which every server's verification uses; nullptr when
    // its login can't be made, `no_pool` saying why (engine::ssh_target).
    std::shared_ptr<ssh::Pool> pool;
    std::string no_pool;
    // The pool is only for verifying (engine = rclone): its idle connections close when the check
    // ends, as rclone check's did, so rclone's downloads in the freed seats stay under the cap.
    bool close_idle_after = false;
    std::string remote_dir; // the server folder the files' paths are relative to; "": as they are
    std::vector<File> files;
    // The seats the check takes from the server's budget: one for a file, up to the download's
    // share for a folder (no more than the hash commands the pool runs at once).
    fetch::Connections* connections = nullptr;
    std::string server;
    int id = 0;
    bool joined = false; // the caller is in the budget under `id` already
    bool many = false;
    engine::CommandHook on_command; // -v: each md5sum command, as "exec md5sum -z <path>"
    // Each md5sum command's time: this, and its bytes at 10 MiB/s (ssh::hash_deadline).
    std::chrono::milliseconds deadline{std::chrono::minutes(5)};
    // Each file's result as soon as it is known (one call at a time, from the check's threads):
    // a match or an unchecked one as its command and its local sum end, a mismatch once proven
    // (at the end). `done` of `files.size()` have their result.
    std::function<void(std::size_t file, const Result& result, std::size_t done)> on_file;
    // After each on_file: how many files have their result, of how many (the row's "12 of 40").
    std::function<void(std::size_t done, std::size_t total)> on_progress;
};

struct Report {
    std::vector<Result> files; // in Request::files' order
    [[nodiscard]] std::size_t count(Verdict v) const;
};

// Runs the check: every file gets a result, a stop leaves the rest unchecked ("stopped").
[[nodiscard]] Report run(const Request& r, std::stop_token stop);
// One downloaded file, `copy.path` being its path on the server; `base` gives the server, the pool
// and the hooks.
[[nodiscard]] Report file(Request base, File copy, std::stop_token stop);

// For the queue row and the command: "checksum verified (MD5)", or for a folder's files "3 files
// verified (MD5)", or why not ("not verified: the server can't compute checksums (...)").
[[nodiscard]] std::string summary(const Report& r, bool folder = false);

// The copy's look after a differing sum (exposed for tests): whether its size and time are what
// the download left (`size`, and `modtime` within 2 s, as FAT keeps times).
[[nodiscard]] bool copy_unchanged(const std::filesystem::path& local, std::uint64_t size, std::string_view modtime);

} // namespace grab::verify
