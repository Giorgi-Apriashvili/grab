#pragma once

// What grab-gui remembers between runs (%APPDATA%\grab\gui.json). Pure data + JSON, so it
// lives in grab_core and is unit-tested.

#include "json.hpp"
#include "schedule.hpp"

#include <cstdint>
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
    std::map<std::string, int> server_limits;        // remote -> connection budget learned from refusals
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

[[nodiscard]] std::filesystem::path default_gui_state_path(); // <config>/grab/gui.json

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

// An unfinished download, kept across restarts in <config>/grab/queue.json.
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

[[nodiscard]] std::filesystem::path default_queue_path(); // <config>/grab/queue.json
[[nodiscard]] std::vector<SavedDownload> parse_queue(std::string_view json_text); // bad entries skipped
[[nodiscard]] std::string queue_to_json(const std::vector<SavedDownload>& items);

// Unknown or malformed content yields defaults for the affected fields, never an error: a
// broken state file must not keep the GUI from starting.
[[nodiscard]] GuiState parse_gui_state(std::string_view json_text);
[[nodiscard]] std::string gui_state_to_json(const GuiState& state);

[[nodiscard]] GuiState load_gui_state(const std::filesystem::path& p);
bool save_gui_state(const std::filesystem::path& p, const GuiState& state);

} // namespace grab
