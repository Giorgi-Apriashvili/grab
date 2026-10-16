#pragma once

// Saved file lists: every file and folder a server's search looks at (its search roots, down to
// max_depth, dot-entries left out as searches leave them out), fetched in one go and kept on
// this PC. A search can then answer from the list in milliseconds instead of walking the server
// again, which on a Storage Box takes 8-18 s. Both front ends use them; a list older than
// fresh_for is fetched again (the command) or refreshed in the background (the window).

#include "engine.hpp"
#include "process.hpp"

#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grab::file_list {

inline constexpr std::chrono::minutes fresh_for{15};

struct Entry {
    std::string path; // as a search returns it, ready for rclone
    bool dir = false;
    std::optional<std::uint64_t> size; // files
    std::optional<std::int64_t> modified{}; // Unix seconds
    bool operator==(const Entry&) const = default;
};

struct List {
    std::int64_t fetched = 0; // Unix seconds
    std::string fingerprint;  // what it covers (see fingerprint())
    std::vector<Entry> entries;
    bool dated = true; // false: saved by an older grab, without dates, so due for a refresh
    bool operator==(const List&) const = default;
};

// What a list covers: the server's address and login, lookup method, search roots, depth and
// whether dot-entries are skipped. A list whose fingerprint differs is stale.
[[nodiscard]] std::string fingerprint(const engine::Context& ctx);

// Whether a list made at `fetched` is still fresh at `now` (both Unix seconds). A clock that
// went backwards makes it stale.
[[nodiscard]] bool fresh(std::int64_t fetched, std::int64_t now);
// The same for a list, which without dates (an older grab's) is never fresh.
[[nodiscard]] bool fresh(const List& list, std::int64_t now);

// Whether a list can answer `req`. Not: a path target (it may lie deeper than the list), a
// search for dot-names (lists skip them), or a depth other than the list's.
[[nodiscard]] bool can_answer(const engine::Context& ctx, const engine::SearchRequest& req);

// Answers `req` from `list` as engine::search would from the server: the same matching and
// ranking, files or folders by req.mode.
[[nodiscard]] engine::SearchResult search(const engine::Context& ctx, const List& list,
                                          const engine::SearchRequest& req);

// A hit of a search over several servers.
struct ServerHit {
    std::string server;
    engine::Hit hit;
};
// Merges several servers' hits into one ranking, ordered as rank_matches orders one server's
// (how well the name matches, then shallower first, then by path); the server name breaks ties.
void rank_across(std::vector<ServerHit>& hits, const engine::SearchRequest& req);

// The `find` command that lists everything under `roots` for the ssh lookup: one
// "d\t<mtime>\t<path>\0" or "f\t<size>\t<mtime>\t<path>\0" record per entry (<mtime>: %T@).
[[nodiscard]] std::string find_command(const std::vector<std::string>& roots, int max_depth, bool skip_hidden);
[[nodiscard]] std::vector<Entry> parse_find(std::string_view out);
// `rclone lsf --format tsp --time-format unix -R` output of `root` (see parse_lsf_line).
[[nodiscard]] std::vector<Entry> parse_lsf(std::string_view out, std::string_view root);

// Fetches the server's list now (ssh + find, or rclone lsf per root). `batch_ssh`: never let
// ssh prompt (the window).
[[nodiscard]] std::expected<List, std::string> fetch(const engine::Context& ctx, std::int64_t now, bool batch_ssh = false,
                                                     const proc::RunOptions& run = {},
                                                     const engine::CommandHook& on_command = {});

// %LOCALAPPDATA%\grab\lists (Windows), else $XDG_CACHE_HOME or ~/.cache, /grab/lists.
[[nodiscard]] std::filesystem::path default_dir();
// <dir>\<server>.list; a name with characters unsafe in a file name gets them replaced and a
// short hash appended, so different names never share a file.
[[nodiscard]] std::filesystem::path path_for(const std::filesystem::path& dir, std::string_view server);

// The text form, version 2: a header, then one line per entry ("d\t<mtime>\t<path>",
// "f\t<size>\t<mtime>\t<path>", a field left empty when unknown), with tabs, line breaks and
// backslashes in names escaped. from_text also reads version 1, which has no <mtime>: such a
// list is not `dated`.
[[nodiscard]] std::string to_text(const List& list);
[[nodiscard]] std::optional<List> from_text(std::string_view text); // nullopt: not a list
// A list file holds the text form encrypted for the current Windows user (DPAPI). load()
// returns nullopt for anything else (an unencrypted or damaged file, another user's), and the
// list is then fetched again.
[[nodiscard]] std::optional<List> load(const std::filesystem::path& file);
bool save(const std::filesystem::path& file, const List& list); // atomically: a sibling, then rename

} // namespace grab::file_list
