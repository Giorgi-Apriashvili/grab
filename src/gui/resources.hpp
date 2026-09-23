#pragma once

#include <optional>
#include <string_view>

namespace grab::gui {

// A UI file compiled into grab-gui.exe (see grab-gui.rc), served at https://grab.ui/<path>.
struct EmbeddedFile {
    std::string_view data;
    const wchar_t* content_type; // for the Content-Type header
};

// `path` is the URL path, e.g. L"/index.html". nullopt for anything not embedded.
[[nodiscard]] std::optional<EmbeddedFile> find_embedded(std::wstring_view path);

} // namespace grab::gui
