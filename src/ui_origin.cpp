#include "ui_origin.hpp"

#include <cstddef>

namespace grab::ui_origin {

namespace {

constexpr std::wstring_view origin_slash = L"https://grab.ui/";
constexpr std::wstring_view preview_prefix = L"https://grab.ui/preview/";

bool starts_with_nocase(std::wstring_view s, std::wstring_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        wchar_t c = s[i];
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
        if (c != prefix[i]) return false;
    }
    return true;
}

// Characters that would hide or reorder what the address says.
bool misleading(wchar_t c) {
    return c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x061C || c == 0x200E || c == 0x200F ||
           (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069);
}

} // namespace

bool is_page(std::wstring_view uri) {
    return uri == page || (uri.starts_with(page) && uri[page.size()] == L'#');
}

std::optional<std::wstring_view> path_of(std::wstring_view uri) {
    if (!uri.starts_with(origin_slash)) return std::nullopt;
    uri.remove_prefix(origin_slash.size() - 1); // keep the '/'
    return uri.substr(0, uri.find_first_of(L"?#"));
}

bool is_preview(std::wstring_view uri) { return uri.starts_with(preview_prefix); }

bool is_web_link(std::wstring_view uri) {
    return (starts_with_nocase(uri, L"https://") || starts_with_nocase(uri, L"http://")) && !path_of(uri);
}

std::wstring link_host(std::wstring_view uri) {
    const auto scheme = uri.find(L"://");
    if (scheme == std::wstring_view::npos) return {};
    auto authority = uri.substr(scheme + 3);
    authority = authority.substr(0, authority.find_first_of(L"/?#\\"));
    if (const auto at = authority.rfind(L'@'); at != std::wstring_view::npos) authority.remove_prefix(at + 1);
    if (authority.starts_with(L'[')) { // an IPv6 address: [::1]:8080
        const auto close = authority.find(L']');
        return std::wstring(authority.substr(0, close == std::wstring_view::npos ? authority.size() : close + 1));
    }
    return std::wstring(authority.substr(0, authority.find(L':')));
}

std::wstring shown(std::wstring_view uri, std::size_t max) {
    std::wstring out;
    for (const wchar_t c : uri.substr(0, max)) out += misleading(c) ? L'�' : c;
    if (uri.size() > max) out += L'…';
    return out;
}

} // namespace grab::ui_origin
