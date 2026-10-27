#pragma once

// Searches and file lists through grab's own SSH client with the server's GNU find, run over an
// exec channel on a pooled connection instead of ssh.exe (RCLONE-REPLACEMENT.md, phase 4). Which
// servers get find is decided once per program run by a short check (the probe); the others are
// walked over SFTP (ssh_walk.hpp).
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

// The check: exactly "grab-find-ok\n" with exit 0 means a GNU find grab can use (-L, -printf).
// Anything else means walking: an SFTP-only account (nothing, exit 0), a restricted shell or a
// Storage Box ("Command not found"), no find (exit 127), a refused command, a shell that prints a
// banner first (review G61), BusyBox or BSD find, Windows. About 3 round trips.
inline constexpr std::string_view find_probe = "env LC_ALL=C find -L . -maxdepth 0 -printf 'grab-find-ok\\n'";
inline constexpr std::chrono::seconds find_probe_deadline{10};

// What the server's answer to find_probe means: `ran` is what exec gave, `out` its stdout.
[[nodiscard]] FindCheck read_find_probe(const std::expected<ExecResult, ExecError>& ran, std::string_view out);

// The probe on one of the pool's connections, asked once per pool (Pool::find_check). nullopt
// when stopped. Throws Error when no login works (`retry`: as Pool::acquire). The caller holds
// none of the pool's leases (with a pool of one, it would wait for its own).
[[nodiscard]] std::optional<FindCheck> check_find(Pool& pool, std::stop_token stop, Retry retry = std::nullopt);

// `method` as the server's answer bears it out (a Lookup, ssh_pool.hpp): probe uses find where
// the check says it works and walks elsewhere; find needs it to work, else it's an error naming
// the server's answer; walk asks nothing. An error too when the check's login fails, or "stopped".
[[nodiscard]] std::expected<Lookup, std::string> choose_lookup(Pool& pool, GrabMethod method, std::string_view server,
                                                               std::stop_token stop, Retry retry = std::nullopt);

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

// What find's stderr and exit status mean for its results (exposed for tests). Exit 0, or exit 1
// with only lines about folders it couldn't read ("Permission denied", "No such file or
// directory") and link loops ("File system loop detected", "Too many levels of symbolic links"),
// keep the results; those lines become notes ("a/b: permission denied"). A root among them doesn't
// stop the other roots, but every root failing is an error; so is anything else, with the first
// line of stderr. `cut`: `err` is only the start of stderr (ExecResult::err_cut), so its last
// line, most likely cut short, is left out and a note says more wasn't kept.
[[nodiscard]] std::expected<std::vector<std::string>, std::string>
read_find_errors(const FindSpec& spec, std::string_view err, int status, bool cut = false);

// Runs find_command(spec) on one of the pool's connections, its output parsed as it streams.
// Empty when stopped. Past max_entries records or max_bytes it's stopped and an error ("listing
// too large"), and the connection isn't used again.
[[nodiscard]] std::expected<Found, std::string> run_find(Pool& pool, const FindSpec& spec, const FindOptions& options);

} // namespace grab::ssh
