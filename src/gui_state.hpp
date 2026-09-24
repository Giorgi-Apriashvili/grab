#pragma once

// What grab-gui remembers between runs (%APPDATA%\grab\gui.json). Pure data + JSON, so it
// lives in grab_core and is unit-tested.

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

inline constexpr int min_parallel = 1;
inline constexpr int max_parallel = 8;
inline constexpr double min_limit_mibps = 0.1;
inline constexpr double max_limit_mibps = 10000;

struct GuiState {
    std::optional<WindowPlacement> window;
    std::string last_remote;
    std::string mode = "file"; // "file" | "folder"
    std::map<std::string, std::string> destinations; // remote -> last destination folder
    int parallel = 4;                                // downloads at once, min_parallel..max_parallel
    std::map<std::string, int> server_limits;        // remote -> connection budget learned from refusals
    bool limit_on = false;                           // global download speed limit
    double limit_mibps = 5.0;                        // its value, kept while switched off
    bool operator==(const GuiState&) const = default;
};

[[nodiscard]] std::filesystem::path default_gui_state_path(); // <config>/grab/gui.json

// One file of a folder download, as saved.
struct SavedFile {
    std::string path; // relative to the folder
    std::uint64_t size = 0;
    std::string modtime;
    std::string state = "queued"; // "queued" | "paused" | "done" | "skipped"
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
    bool operator==(const SavedDownload&) const = default;
};

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
