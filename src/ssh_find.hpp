#pragma once

// Searches and file lists through grab's own SSH client with the server's GNU find, run over an
// exec channel on a pooled connection instead of ssh.exe (RCLONE-REPLACEMENT.md, phase 4). Which
// servers get find is decided by a short check (the probe), asked once a week per login, its
// answer saved across runs (FindMemory), and again at once when find fails as it does where
// there's none; the others are walked over SFTP (ssh_walk.hpp).
//
// find runs as `env LC_ALL=C find -L`: links are followed as the walk follows them (review G54),
// and the C locale makes its messages and quote marks predictable and -iname fold only ASCII, as
// grab's own matching does. Paths go out as the server's raw bytes and come back in grab's form
// (rclone_enc), as the walk gives them. stderr is read rather than thrown away (review G55).

#include "config.hpp"
#include "file_list.hpp"
#include "match.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace grab::ssh {

// The check: exactly find_probe_ok with exit 0 means a GNU find grab can use (-L, -printf) under a
// shell that reads single quotes as POSIX sh does. Anything else means walking: an SFTP-only
// account (nothing, exit 0), a restricted shell or a Storage Box ("Command not found"), no find
// (exit 127), a refused command, a shell that prints a banner first (review G61), BusyBox or BSD
// find, Windows. About 3 round trips.
//
// find_command puts names from the server inside single quotes (quote::sh_single), where a POSIX
// shell takes every byte as it is. fish reads \\ and \' as escapes even there, so a name ending in
// a backslash, or holding \', would end its quotes early and run the rest (a hostile folder name
// from Browse). The probe's format holds both: sh hands find `grab-find-ok\\\n\c\` (find prints
// "grab-find-ok\" and a line break, then stops at \c), while fish sees \\ as one backslash and \'
// as a quote that doesn't close (a syntax error), so such a server is walked. pwsh (a Linux login
// shell too) also ends single quotes at ‘ ’ ‚ ‛ (U+2018 to U+201B), so a name holding one could
// end them as well: the probe's ’ after \c ends pwsh's string, and its last ' opens one that
// doesn't close. sh hands find the ’ after \c, which it never prints. The ’ is spelled in bytes
// (UTF-8), so the probe doesn't depend on the compiler's source character set.
inline constexpr std::string_view find_probe =
    R"(env LC_ALL=C find -L . -maxdepth 0 -printf 'grab-find-ok\\\n\c)" "\xE2\x80\x99" R"(\')";
inline constexpr std::string_view find_probe_ok = "grab-find-ok\\\n";
inline constexpr std::chrono::seconds find_probe_deadline{10};

// What the server's answer to find_probe means: `ran` is what exec gave, `out` its stdout.
[[nodiscard]] FindCheck read_find_probe(const std::expected<ExecResult, ExecError>& ran, std::string_view out);

// The probe on one of the pool's connections, asked once per pool (Pool::find_check) and only
// when `memory` has no answer; a new lasting answer goes to `memory`. nullopt when stopped.
// Throws Error when no login works (`retry`: as Pool::acquire). The caller holds none of the
// pool's leases (with a pool of one, it would wait for its own).
[[nodiscard]] std::optional<FindCheck> check_find(Pool& pool, std::stop_token stop, Retry retry = std::nullopt,
                                                  const FindMemory& memory = {});

// `method` as the server's answer bears it out (a Lookup, ssh_pool.hpp): probe uses find where
// the check says it works and walks elsewhere; find needs it to work, else it's an error naming
// the server's answer (one an earlier run saved is asked again first: the server may have changed);
// walk asks nothing. An error too when the check's login fails, or "stopped".
[[nodiscard]] std::expected<Lookup, std::string> choose_lookup(Pool& pool, GrabMethod method, std::string_view server,
                                                               std::stop_token stop, Retry retry = std::nullopt,
                                                               const FindMemory& memory = {});
// After find failed as it does where there's no find for grab (FindFailed::about_find) on an
// answer known from before (a saved one, or one asked earlier in the program): drops that answer
// from the pool and from `memory`, then asks the check again at once, as choose_lookup.
[[nodiscard]] std::expected<Lookup, std::string> recheck_lookup(Pool& pool, GrabMethod method, std::string_view server,
                                                                std::stop_token stop, Retry retry = std::nullopt,
                                                                const FindMemory& memory = {});

// What a find on grab's engine looks for.
struct FindSpec {
    std::vector<std::string> roots; // search roots in grab's form; "" = the login folder
    int max_depth = 4;
    bool skip_hidden = true;    // dot-names neither printed nor entered, unless `query` wants them
    std::optional<Query> query; // a name search, filtered on the server; none: every entry (a list)
    bool folders = true;        // print folders
    bool files = true;          // print files
};

// `env LC_ALL=C find -L <roots> -mindepth 1 -maxdepth N ...`, each entry printed as a NUL-ended
// file_list record ("d\t<mtime>\t<path>", "f\t<size>\t<mtime>\t<path>"), with no `2>/dev/null`.
// Roots go as raw bytes, quoted for the shell, a relative one after "./" so no name is taken for
// an option; so do the search words.
[[nodiscard]] std::string find_command(const FindSpec& spec);

struct FindOptions {
    std::stop_token stop;
    std::size_t max_entries = sftp::max_listing;       // records past which find is stopped (review G168)
    std::uint64_t max_bytes = sftp::max_listing_bytes; // ...and bytes of the paths kept
    std::function<void(std::size_t)> progress;   // records so far, as the output arrives
    Retry retry_refused = std::nullopt;          // for its login (Pool::acquire)
};

struct Found {
    std::vector<file_list::Entry> entries; // in find's order, paths in grab's form as a search gives them
    std::vector<std::string> notes;        // as engine::SearchResult::notes
};

// Why find failed, and whether that says the server has no find for grab (any more) rather than
// something about this search: the command refused, exit 126 or 127, no exit status, a line such
// as "command not found" or a complaint about an option find_command uses, output that isn't
// find's records (a banner, a restricted shell's answer). Then a check's answer known from
// before is worth asking again (recheck_lookup). A login, a lost connection, a signal, a cap, or
// roots find couldn't read are not about find.
struct FindFailed {
    std::string message;
    bool about_find = false;
};

// What find's stderr and exit status mean for its results (exposed for tests). Exit 0, or exit 1
// with only lines about folders it couldn't read ("Permission denied", "No such file or
// directory") and link loops ("File system loop detected", "Too many levels of symbolic links"),
// keep the results; those lines become notes ("a/b: permission denied"). A root among them doesn't
// stop the other roots, but every root failing is an error. At exit 1 so is any other line (the
// first one is the message), marked about_find when it names no path (doesn't start with
// "find: '"); at exit 0 such lines are notes. Any other exit status fails it, about find for 126,
// 127, no status and other codes find doesn't give, not for 128 and up (a signal). `cut`: `err` is only
// the start of stderr (ExecResult::err_cut), so its last line, most likely cut short, is left out
// and a note says more wasn't kept.
[[nodiscard]] std::expected<std::vector<std::string>, FindFailed>
read_find_errors(const FindSpec& spec, std::string_view err, int status, bool cut = false);

// Runs find_command(spec) on one of the pool's connections, its output parsed as it streams.
// Empty when stopped. Past max_entries records or max_bytes it's stopped and an error ("listing
// too large"), and the connection isn't used again.
[[nodiscard]] std::expected<Found, FindFailed> run_find(Pool& pool, const FindSpec& spec, const FindOptions& options);

} // namespace grab::ssh
