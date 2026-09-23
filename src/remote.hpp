#pragma once

#include "cli.hpp"
#include "config.hpp"
#include "match.hpp"

#include <cstddef>
#include <expected>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace grab {

struct FindRequest {
    Mode mode = Mode::folder;
    std::string target;              // search words / pattern, or a remote path containing '/'
    bool exact = false;              // --exact: whole-name, case-sensitive (see match.hpp)
    std::vector<std::string> roots;  // "" = login home; ignored when target is a path
    int max_depth = 4;
    bool skip_hidden = true;
};

// A target with a '/' is a path (absolute, or relative to the login home) to be checked
// rather than a name to be searched for.
[[nodiscard]] bool is_path_target(std::string_view target);

// The `find ... -print0` command run by the remote shell. Name matching happens on the
// server: one `-iname '*word*'` per word, `-iname` for a glob, `-name` for --exact.
[[nodiscard]] std::string build_find_command(const FindRequest& req);

// ssh argv: options first, then user@host, then the remote command as one argument.
[[nodiscard]] std::vector<std::string> build_ssh_argv(const std::string& ssh_exe,
                                                      const RcloneRemote& remote,
                                                      std::span<const std::string> ssh_options,
                                                      const std::string& remote_command);

// Split NUL-separated find output into paths; a leading "./" (from a home-relative
// search) is dropped.
[[nodiscard]] std::vector<std::string> parse_find_output(std::string_view out);

// Best first: match tier (exact name, then name starting with the first word), then
// shallowest, then case-insensitive path order (so episodes list in order).
void rank_matches(std::vector<std::string>& matches, const Query& query);

// A pick answer against `count` listed items: "3", "1-5,8", "5-3", "2 4", "a"/"all".
// Returns zero-based indices in first-seen order without duplicates.
[[nodiscard]] std::expected<std::vector<std::size_t>, std::string>
parse_selection(std::string_view answer, std::size_t count);

struct PickOptions {
    bool first = false;       // --first: take the best match
    bool all = false;         // --all: take every match
    bool interactive = false; // stdin is a terminal, so asking is possible
};

// Choose what to download from the (unranked) matches: the only one, --first, --all, or ask
// on `err`/`in` with a numbered list. Enter picks [1].
[[nodiscard]] std::expected<std::vector<std::string>, std::string>
choose_matches(std::vector<std::string> matches, const Query& query, const PickOptions& pick,
               std::istream& in, std::ostream& err);

} // namespace grab
