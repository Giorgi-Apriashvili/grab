#pragma once

#include "cli.hpp"
#include "config.hpp"
#include "match.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace grab {

// One lookup result: a remote path and, when the lookup reports it, its size in bytes
// (files only; folders have no cheap size).
struct RemoteEntry {
    std::string path;
    std::optional<std::uint64_t> size;
    bool operator==(const RemoteEntry&) const = default;
};

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

// The `find` command run by the remote shell. Name matching happens on the server: one
// `-iname '*word*'` per word, `-iname` for a glob, `-name` for --exact. Files are printed
// as "<size>\t<path>\0" (GNU find -printf), folders as "<path>\0".
[[nodiscard]] std::string build_find_command(const FindRequest& req);

// ssh argv: options first, then user@host, then the remote command as one argument.
[[nodiscard]] std::vector<std::string> build_ssh_argv(const std::string& ssh_exe,
                                                      const RcloneRemote& remote,
                                                      std::span<const std::string> ssh_options,
                                                      const std::string& remote_command);

// Split NUL-separated find output into entries; `sized` records are "<size>\t<path>". A
// leading "./" (from a home-relative search) is dropped.
[[nodiscard]] std::vector<RemoteEntry> parse_find_output(std::string_view out, bool sized);

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
// on `err`/`in` with a numbered list. Enter picks [1]. `describe`, if set, returns extra text
// shown after each listed path (e.g. its size).
[[nodiscard]] std::expected<std::vector<std::string>, std::string>
choose_matches(std::vector<std::string> matches, const Query& query, const PickOptions& pick,
               std::istream& in, std::ostream& err,
               const std::function<std::string(const std::string&)>& describe = {});

// Ask where to save: prints the prompt on `err`, reads one line from `in`. Enter means
// `fallback`; surrounding quotes from a pasted path are dropped.
[[nodiscard]] std::expected<std::filesystem::path, std::string>
ask_destination(std::istream& in, std::ostream& err, const std::filesystem::path& fallback);

} // namespace grab
