#pragma once

// What grab-gui's window may show and whom it talks to (review finding G40, 2026-10-21). The
// page at https://grab.ui/index.html has a bridge to grab (window.chrome.webview) that can add
// servers, start downloads anywhere and read the configuration, and WebView2 gives that bridge to
// whatever document is on top. A bare prefix check let https://grab.ui@example.com/ (or
// grab.ui.example.com) take the window over from one click on a link in a previewed PDF. So
// addresses are compared exactly, and links elsewhere only open in the browser after asking.

#include <optional>
#include <string>
#include <string_view>

namespace grab::ui_origin {

// The window's own page: the only document it shows and the only one whose messages count.
inline constexpr std::wstring_view page = L"https://grab.ui/index.html";

// Whether `uri` is the window's own page (a #fragment allowed; nothing else: no other path,
// query, host, port or user@ part).
[[nodiscard]] bool is_page(std::wstring_view uri);
// The path of one of grab's own resources ("/app.js" for https://grab.ui/app.js?x), without
// query or fragment; nullopt for an address anywhere else.
[[nodiscard]] std::optional<std::wstring_view> path_of(std::wstring_view uri);
// Whether the viewer's frame may show `uri`: grab's previews only.
[[nodiscard]] bool is_preview(std::wstring_view uri);
// Whether `uri` is a web page elsewhere (http or https, not grab's own), which a link in a
// preview may open in the browser once the user agrees.
[[nodiscard]] bool is_web_link(std::wstring_view uri);
// The host a web link really goes to: after any user@ part, without the port
// ("example.com" for https://grab.ui@example.com:8443/x).
[[nodiscard]] std::wstring link_host(std::wstring_view uri);
// `uri` as the question shows it: control and direction-changing characters as U+FFFD, at most
// `max` characters (then "…").
[[nodiscard]] std::wstring shown(std::wstring_view uri, std::size_t max = 300);

} // namespace grab::ui_origin
