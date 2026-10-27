#pragma once

// The server's MD5 of downloaded files, for verification (RCLONE-REPLACEMENT.md, phase 5): md5sum
// run over an exec channel on the server's pooled connections, in place of `rclone check` and
// `rclone hashsum md5`. Each file goes to md5sum by its absolute path (the login folder from SFTP's
// REALPATH, as rclone builds them), its raw bytes in single quotes, several to a command, and a
// sum counts only when its record names the path sent, byte for byte, in the order sent: a shell's
// banner, a record grab can't read or a file md5sum couldn't read leaves the file unchecked, never
// "different".
//
// Which servers have an md5sum grab can use is a short check (the probe), asked once per pool and
// not saved: `md5sum -z ''\-` gives md5sum's record for empty stdin only where a POSIX shell took
// the quotes and the backslash away, so a Windows server (cmd and PowerShell keep the backslash:
// "no such file \-"; the class of bug rclone fixed in 1.75.0, CVE-2026-71312), a Storage Box or a
// restricted shell without md5sum, an SFTP-only account or a shell that prints a banner (review
// G61) fails it before any name is sent.
//
// md5sum is silent until it ends (its stdout to a pipe is block-buffered), for minutes on big
// files: the exec's keepalives (every 15 s, the link dead after 45 s without an answer) keep it
// alive, as Main and a Storage Box both answer them. No `env`, pipes or redirects: a Storage Box's
// restricted shell has none, and md5sum's records don't depend on the locale.

#include "config.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace grab::ssh {

// The check, with empty stdin (exec sends EOF at once): -z, then without it where -z fails.
inline constexpr std::string_view hash_probe = "md5sum -z ''\\-";
inline constexpr std::string_view hash_probe_plain = "md5sum ''\\-";
inline constexpr std::chrono::seconds hash_probe_deadline{10};

// What the server's answer to the probe (hash_probe with `z`, else hash_probe_plain) means: `ran`
// is what exec gave, `out` its stdout. It works only on exactly md5sum's record for empty stdin,
// "d41d8cd98f00b204e9800998ecf8427e  -" and a NUL (-z) or a line break, with exit 0. A refusal,
// no answer in time or an end by a signal may pass (not lasting). `home` stays empty.
[[nodiscard]] HashCheck read_hash_probe(const std::expected<ExecResult, ExecError>& ran, std::string_view out, bool z);

// The check on the pool's connections, asked once per pool (Pool::hash_check): the login folder
// over SFTP, then the probe on the same login. A probe refused there (MaxSessions 1: the server
// allows no command beside SFTP) is asked once more on a new login (Pool::acquire_new). A login
// that fails is the answer (HashFailure::login), so a program tries one, never one per file:
// failed logins count with fail2ban. nullopt when stopped. The caller holds none of the pool's
// leases.
[[nodiscard]] std::optional<HashCheck> hash_check(Pool& pool, std::stop_token stop, Retry retry = std::nullopt);

// Why a file has no server sum, in a few words for "not verified: <why>".
struct HashFailed {
    HashFailure kind = HashFailure::unpaired;
    std::string why;
    bool operator==(const HashFailed&) const = default;
};
// A file's sum on the server (32 lower-case hex digits), or why there's none.
using HashResult = std::expected<std::string, HashFailed>;

// Whether the keys rclone saved for the server in rclone.conf keep grab's check from it, without
// running anything: shell_type cmd, powershell or none, or md5sum_command none (rclone found no
// way to hash there); another md5sum_command or any path_override (not supported by grab's check
// yet). nullopt when the check may run.
[[nodiscard]] std::optional<HashFailed> hash_blocked(const RcloneRemote& remote);

// A raw path (from the login folder, or absolute) as md5sum gets it: absolute, after `home`.
[[nodiscard]] std::string absolute_path(std::string_view home, std::string_view raw);

// `md5sum [-z] '<path>' ...`: absolute paths (they can't start with '-', so no `--`) as raw bytes
// in single quotes, which a POSIX shell takes as they are.
[[nodiscard]] std::string hash_command(std::span<const std::string> paths, bool z);

// What one md5sum command may hold.
struct BatchLimits {
    std::size_t files = 256;
    // Bytes of the command line: libssh2 sends no exec request with a command past 34,725 bytes (it
    // fails on grab's side and loses the connection; measured on Main and a Storage Box), whatever
    // the server would take.
    std::size_t command = std::size_t{32} * 1024;
    std::uint64_t bytes = std::uint64_t{1} << 30;   // of the files
    std::uint64_t alone = std::uint64_t{256} << 20; // a file this big gets a command of its own
};

// One file to hash: its raw path on the server (from the login folder, or absolute) and size.
struct HashFile {
    std::string path;
    std::uint64_t size = 0;
};

// `files` (absolute paths, as md5sum gets them) as md5sum commands, each a list of indices into
// `files`: the biggest files first, as they decide how long it all takes; each command within
// `limits`; and without -z, a name holding a backslash, CR or LF alone, so that a record another
// md5sum splits or spells differently can't pair with a name sent beside it. A file whose command
// alone is longer than limits.command still gets one (run_hashes never sends it).
[[nodiscard]] std::vector<std::vector<std::size_t>> plan_batches(std::span<const HashFile> files, bool z,
                                                                 const BatchLimits& limits = {});

// The most stdout md5sum gives for `paths` (ExecOptions::out_limit, review G168): for each, a
// digest, two spaces and its end (36 bytes) and the name, twice its length without -z (GNU
// escapes a backslash and a line break).
[[nodiscard]] std::uint64_t hash_out_limit(std::span<const std::string> paths, bool z);

// How long a command hashing `bytes` may take: `base` plus the bytes at 10 MiB/s (only a hung
// server should reach it), and never so long that adding it to a clock overflows (review G166): 30
// days at most.
[[nodiscard]] std::chrono::milliseconds hash_deadline(std::uint64_t bytes,
                                                      std::chrono::milliseconds base = std::chrono::minutes(5));

// md5sum's output for the paths `sent` (exactly as the command named them, in its order): for each
// path its sum, or nullopt (unchecked). A record counts only when it names the next paths sent, or
// one after them (md5sum gives none for a file it can't read), byte for byte: with -z
// "<digest>  <name>" ended by a NUL; without, ended by a line break, and GNU's form of a name
// holding a backslash or line break too: '\' before the record, the name's \\, \n (and \r in newer
// coreutils) undone. A record of another shape, a name not sent or out of order, and the cut end of
// the output pair with nothing.
[[nodiscard]] std::vector<std::optional<std::string>> parse_sums_in_order(std::string_view out,
                                                                         std::span<const std::string> sent, bool z);

struct HashOptions {
    std::stop_token stop;
    int connections = 1; // commands at once for this run (its seats), within the pool's hashes_at_once
    // The server as rclone.conf has it: its keys (hash_blocked); on a Storage Box (its host), no name
    // holding a control byte goes to the shell until it is known how the box's shell reads a line
    // break inside quotes.
    RcloneRemote server;
    BatchLimits batches;
    std::chrono::milliseconds deadline{std::chrono::minutes(5)}; // each command's: this, and its bytes at 10 MiB/s
    Retry retry_refused = std::nullopt;                          // for its logins (Pool::acquire)
    // Each file's result as its command ends (one call at a time, from the run's threads).
    std::function<void(std::size_t file, const HashResult& result)> done;
};

// The server's MD5 of each of `files`, in their order: hash_blocked, then the pool's check, then
// md5sum commands (plan_batches) on the pool's connections, each within a hash turn
// (Pool::hash_turn), a deadline (hash_deadline) and hash_out_limit. A command refused on a reused
// connection is tried once on a new login (Pool::acquire_new); one on a reused connection that
// breaks, on the next. A login that fails ends the run's other commands too, and the pool keeps it
// as the check's answer (Pool::set_hash_check), so later runs don't try again. A stop, the
// deadline or a lost connection costs the files of the commands then running: they are "not
// verified", as is every file the check or the rules above leave out.
[[nodiscard]] std::vector<HashResult> run_hashes(Pool& pool, std::span<const HashFile> files,
                                                 const HashOptions& options = {});

} // namespace grab::ssh
