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

// One lookup result: a remote path and, when the lookup reports them, its size in bytes
// (files only; folders have no cheap size) and modification time.
struct RemoteEntry {
    std::string path;
    std::optional<std::uint64_t> size;
    std::optional<std::int64_t> modified{}; // Unix seconds
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

// The `find -L` command run by the remote shell: links are followed, as rclone's listing and
// grab's walk follow them (review G54). Name matching happens on the server: one
// `-iname '*word*'` per word, `-iname` for a glob, `-name` for --exact. Files are printed
// as "<size>\t<mtime>\t<path>\0" (GNU find -printf), folders as "<mtime>\t<path>\0"; <mtime>
// is %T@, Unix seconds with a fraction.
[[nodiscard]] std::string build_find_command(const FindRequest& req);

// A file for an ssh -o option such as UserKnownHostsFile, as ssh reads it back (review G1): in
// double quotes (a space would end it), '\' as '/' on Windows (ssh takes a backslash for an
// escape) and '%' doubled (ssh expands %d, %u and the like there).
[[nodiscard]] std::string ssh_file_value(std::string_view path);

// ssh argv: options first, then user@host, then the remote command as one argument. With a
// known_hosts_file the host key must be in it (StrictHostKeyChecking=yes), only it is read
// (GlobalKnownHostsFile=NUL) and ssh never adds keys to it (UpdateHostKeys=no, whatever
// ~/.ssh/config says), before `ssh_options` so these come first (review G4). The file goes in as
// ssh_file_value writes it.
[[nodiscard]] std::vector<std::string> build_ssh_argv(const std::string& ssh_exe,
                                                      const RcloneRemote& remote,
                                                      std::span<const std::string> ssh_options,
                                                      const std::string& remote_command);

// Split NUL-separated find output into entries: "[<size>\t]<mtime>\t<path>", the size when
// `sized` (files). A field is taken off only when it parses; a leading "./" (from a
// home-relative search) is dropped.
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

// The text after a listed match: "  (897.9 MiB, 2023-10-15)", "  (2023-10-15)" for a folder,
// "" when neither is known. The date is local.
[[nodiscard]] std::string pick_note(std::optional<std::uint64_t> size, std::optional<std::int64_t> modified);

// Choose what to download from the (unranked) matches: the only one, --first, --all, or ask
// on `err`/`in` with a numbered list. Enter picks [1]. `describe`, if set, returns extra text
// shown after each listed path (e.g. its pick_note).
[[nodiscard]] std::expected<std::vector<std::string>, std::string>
choose_matches(std::vector<std::string> matches, const Query& query, const PickOptions& pick,
               std::istream& in, std::ostream& err,
               const std::function<std::string(const std::string&)>& describe = {});

// Ask where to save: prints the prompt on `err`, reads one line from `in`. Enter means
// `fallback`; surrounding quotes from a pasted path are dropped.
[[nodiscard]] std::expected<std::filesystem::path, std::string>
ask_destination(std::istream& in, std::ostream& err, const std::filesystem::path& fallback);

} // namespace grab
