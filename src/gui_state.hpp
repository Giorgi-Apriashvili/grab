#pragma once

// What grab-gui remembers between runs (%APPDATA%\grab\gui.json). Pure data + JSON, so it
// lives in grab_core and is unit-tested.

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace grab {

struct WindowPlacement {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool maximized = false;
    bool operator==(const WindowPlacement&) const = default;
};

struct GuiState {
    std::optional<WindowPlacement> window;
    std::string last_remote;
    std::string mode = "file"; // "file" | "folder"
    std::map<std::string, std::string> destinations; // remote -> last destination folder
    bool operator==(const GuiState&) const = default;
};

[[nodiscard]] std::filesystem::path default_gui_state_path(); // <config>/grab/gui.json

// Unknown or malformed content yields defaults for the affected fields, never an error: a
// broken state file must not keep the GUI from starting.
[[nodiscard]] GuiState parse_gui_state(std::string_view json_text);
[[nodiscard]] std::string gui_state_to_json(const GuiState& state);

[[nodiscard]] GuiState load_gui_state(const std::filesystem::path& p);
bool save_gui_state(const std::filesystem::path& p, const GuiState& state);

} // namespace grab
