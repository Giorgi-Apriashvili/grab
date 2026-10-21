#pragma once

// What grab-gui remembers between runs (%APPDATA%\grab\gui.json, see state_dir). Pure data +
// JSON, so it lives in grab_core and is unit-tested.

#include "json.hpp"
#include "schedule.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grab {

struct WindowPlacement {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool maximized = false;
    bool operator==(const WindowPlacement&) const = default;
};

// The results' order chosen with a column header.
struct SortOrder {
    std::string key; // "name" | "size" | "modified"
    std::string dir; // "asc" | "desc"
    // With a Size sort, the sort before it ("name" or "modified"), which orders the folders after
    // the files (folders have no size); empty for their default order.
    std::string then_key{};
    std::string then_dir{};
    bool operator==(const SortOrder&) const = default;
};
// {"key":"modified","dir":"desc"}, or {"key":"size","dir":"desc","then":{"key":"name","dir":"asc"}};
// nullopt for anything else. A `then` that isn't a Name or Modified sort is dropped.
[[nodiscard]] std::optional<SortOrder> sort_from_json(const json::Value& v);
[[nodiscard]] json::Value sort_to_json(const SortOrder& s);

inline constexpr int min_parallel = 1;
inline constexpr int max_parallel = 8;
inline constexpr double min_limit_mibps = schedule::min_mibps;
inline constexpr double max_limit_mibps = schedule::max_mibps;

struct GuiState {
    std::optional<WindowPlacement> window;
    std::string last_remote;
    std::map<std::string, std::string> destinations; // remote -> last destination folder
    std::optional<SortOrder> sort;                   // absent: the default order
    int parallel = 4;                               // downloads at once, min_parallel..max_parallel
    // remote -> connection budget learned from refusals, as grab 0.17 and older saved it here; read
    // only, to carry it over to limits.json (learned_limits_path), and no longer written.
    std::map<std::string, int> server_limits;
    // How long steps took ("lsjson:Box.Guest" -> ms, a moving average), for the status bar's
    // progress where nothing can be counted.
    std::map<std::string, int> timings;
    bool verify_downloads = false;                   // check each download against the server's checksum
    bool limit_on = false;                           // global download speed limit
    double limit_mibps = 5.0;                        // its value, kept while switched off
    // Outside the schedule's times the limit above applies; a change made during them holds
    // until the schedule's next change.
    std::vector<schedule::Rule> speed_schedule;
    std::optional<schedule::Override> limit_change;
    bool update_check = true;                        // look for a new release once a day
    std::int64_t last_update_check = 0;              // Unix seconds; 0 = never
    std::string latest_version;                      // the newest release seen ("0.7.0"), if any
    std::string latest_page;                         // its release page
    bool operator==(const GuiState&) const = default;
};

// Where the window keeps gui.json, queue.json and recent for the configuration at `grab_conf`:
// <config>/grab for the default one, <config>/grab/configs/<config_folder_name> for any other
// (GRAB_CONFIG, --config), so two configurations' windows never share a queue (review G35).
[[nodiscard]] std::filesystem::path state_dir(const std::filesystem::path& grab_conf);

// The window's recent searches (state_dir's "recent"): a JSON array of the page's entries, at
// most recent_max, newest first. Sealed for the current Windows user (DPAPI), as the file lists
// are: what was searched for is unreadable to other accounts and to a copy of the file.
inline constexpr std::size_t recent_max = 20;
// An empty array when there is none. One that can't be read (damaged, another user's, DPAPI
// failing for now) is moved aside as "recent.bad-<time>" first, so the next save doesn't
// replace it, and the list starts empty (review G215).
[[nodiscard]] json::Value load_recent(const std::filesystem::path& file);
// Keeps the first recent_max objects (an entry's text at most 4 KiB); none removes the file.
bool save_recent(const std::filesystem::path& file, const json::Value& items);

// One file of a folder download, as saved.
struct SavedFile {
    std::string path; // relative to the folder
    std::uint64_t size = 0;
    std::string modtime;
    std::string state = "queued"; // "queued" | "paused" | "done" | "skipped"
    bool fetched = false;  // done by the download, not found on disk already
    bool verified = false; // checked against the server's checksum
    std::string local = {}; // Keep both: its second-copy name, relative to the folder
    bool operator==(const SavedFile&) const = default;
};

// An unfinished download, kept across restarts in state_dir's queue.json.
struct SavedDownload {
    std::string remote;
    std::string mode = "file"; // "file" | "folder"
    std::string path;
    std::string name;
    std::string dest;
    std::uint64_t total = 0;
    std::vector<SavedFile> files; // a folder's files, once it was listed
    std::string priority = "normal"; // bandwidth priority: "high" | "normal" | "low"
    // Saved for an update's restart while it was running or queued: it continues by itself
    // instead of coming back paused.
    bool resume = false;
    // The free-space check is done: "Download anyway" was chosen, or Resume after it paused it.
    bool space_ok = false;
    // A different file already there: "replace" | "both" | "skip", "" not answered (existing.hpp).
    std::string existing = {};
    std::string local_name = {}; // a file's Keep-both name
    bool operator==(const SavedDownload&) const = default;
};

// Queue order: moves `id` to just before `before` (0 = to the end). Unknown ids, or moving an
// item before itself, change nothing. Returns whether the order changed.
bool move_before(std::vector<int>& ids, int id, int before);

[[nodiscard]] std::vector<SavedDownload> parse_queue(std::string_view json_text); // bad entries skipped
// queue.json as saved: nullopt when there is none; an error when it exists but can't be read or
// isn't a JSON array, so that it is never replaced unseen (review G212).
[[nodiscard]] std::expected<std::optional<std::vector<SavedDownload>>, std::string>
read_queue(const std::filesystem::path& p);
[[nodiscard]] std::string queue_to_json(const std::vector<SavedDownload>& items);

// Unknown or malformed content yields defaults for the affected fields, never an error: a
// broken state file must not keep the GUI from starting.
[[nodiscard]] GuiState parse_gui_state(std::string_view json_text);
[[nodiscard]] std::string gui_state_to_json(const GuiState& state);

// gui.json as saved: nullopt when there is none; an error when it exists but can't be read or
// isn't a JSON object, so that the defaults never replace it unseen (review G213).
[[nodiscard]] std::expected<std::optional<GuiState>, std::string> read_gui_state(const std::filesystem::path& p);
// The connection limits grab learned per server from refusals, shared by the window and the
// command (review G195): state_dir's "limits.json", {"server": n}. `before`: what gui.json held
// as "serverLimits" (grab 0.17 and older), used while the file doesn't exist yet.
[[nodiscard]] std::filesystem::path learned_limits_path(const std::filesystem::path& grab_conf);
[[nodiscard]] std::map<std::string, int> load_learned_limits(const std::filesystem::path& file,
                                                             const std::map<std::string, int>& before = {});
// Sets one server's learned limit, or drops it (nullopt), under the settings lock and from the
// file as it is then: both programs write it.
[[nodiscard]] std::expected<void, std::string> save_learned_limit(const std::filesystem::path& file,
                                                                  const std::string& server, std::optional<int> limit,
                                                                  const std::map<std::string, int>& before = {});

// Replaced in one step (util::write_file_atomic); the error says why it couldn't be.
[[nodiscard]] std::expected<void, std::string> save_gui_state(const std::filesystem::path& p, const GuiState& state);

} // namespace grab
