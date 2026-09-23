#include "resources.hpp"

#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)

namespace grab::gui {

namespace {

struct Entry {
    std::wstring_view path;
    const wchar_t* resource; // RCDATA name in grab-gui.rc
    const wchar_t* content_type;
};

constexpr Entry entries[] = {
    {L"/", L"UI_INDEX_HTML", L"text/html; charset=utf-8"},
    {L"/index.html", L"UI_INDEX_HTML", L"text/html; charset=utf-8"},
    {L"/app.css", L"UI_APP_CSS", L"text/css; charset=utf-8"},
    {L"/app.js", L"UI_APP_JS", L"text/javascript; charset=utf-8"},
};

} // namespace

std::optional<EmbeddedFile> find_embedded(std::wstring_view path) {
    for (const auto& e : entries) {
        if (e.path != path) continue;
        HRSRC res = FindResourceW(nullptr, e.resource, RT_RCDATA);
        if (res == nullptr) return std::nullopt;
        HGLOBAL loaded = LoadResource(nullptr, res);
        const void* data = loaded != nullptr ? LockResource(loaded) : nullptr;
        if (data == nullptr) return std::nullopt;
        return EmbeddedFile{std::string_view(static_cast<const char*>(data), SizeofResource(nullptr, res)),
                            e.content_type};
    }
    return std::nullopt;
}

} // namespace grab::gui
