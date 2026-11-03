#pragma once

#include "match.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
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

// A target with a '/' is a path (absolute, or relative to the login home) to be checked
// rather than a name to be searched for.
[[nodiscard]] bool is_path_target(std::string_view target);

// Last path component, ignoring trailing slashes.
[[nodiscard]] std::string remote_basename(std::string_view remote_path);
// Whether a remote path ends in a name to save it under: not "/", "." or ".." (whose local
// target would be the download folder itself, review G15).
[[nodiscard]] bool has_name(std::string_view remote_path);

// "server:/abs/path": how grab shows a server's path (the server's rclone_remote, then its path).
[[nodiscard]] std::string remote_spec(std::string_view rclone_remote, std::string_view remote_path);

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
