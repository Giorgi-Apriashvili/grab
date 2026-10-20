#include "file_type.hpp"

#include "util.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <format>
#include <system_error>
#include <unordered_map>

namespace grab::file_type {

namespace {

const std::unordered_map<std::string, Kind>& extensions() {
    static const auto table = [] {
        std::unordered_map<std::string, Kind> t;
        auto add = [&](Kind k, std::string_view list) {
            while (!list.empty()) {
                const auto space = list.find(' ');
                t.emplace(std::string(list.substr(0, space)), k);
                list.remove_prefix(space == std::string_view::npos ? list.size() : space + 1);
            }
        };
        add(Kind::video, "mkv mp4 m4v avi mov wmv flv webm ts m2ts mts mpg mpeg vob ogv 3gp divx rmvb");
        add(Kind::audio, "mp3 flac m4a m4b aac ogg oga opus wav wma ape aiff aif alac mka dts ac3 wv");
        add(Kind::image, "jpg jpeg jfif png gif webp avif bmp ico svg tif tiff heic heif psd cr2 nef dng arw");
        add(Kind::text, "nfo txt srt ass ssa vtt log md ini cfg conf json xml csv cue sfv m3u m3u8 yml yaml toml diz "
                        "sub idx html htm rtf");
        add(Kind::document, "pdf epub mobi azw3 djvu doc docx xls xlsx ppt pptx odt ods odp");
        add(Kind::archive, "zip rar 7z tar gz tgz bz2 tbz2 xz txz zst lz lzma cab");
        add(Kind::program, "exe msi msix appx bat cmd ps1 sh apk jar deb rpm");
        add(Kind::disc, "iso img mdf mds nrg dmg vhd vhdx vmdk");
        return t;
    }();
    return table;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

// A split archive's part: .r00, .z01, .001
bool split_part(std::string_view ext) {
    if (ext.size() != 3) return false;
    if ((ext[0] == 'r' || ext[0] == 'z') && is_digit(ext[1]) && is_digit(ext[2])) return true;
    return std::ranges::all_of(ext, is_digit);
}

constexpr Kind all_kinds[] = {Kind::video, Kind::audio, Kind::image, Kind::text, Kind::document,
                              Kind::archive, Kind::program, Kind::disc, Kind::other};

} // namespace

Kind kind_of(std::string_view name) {
    if (const auto slash = name.find_last_of('/'); slash != std::string_view::npos) name.remove_prefix(slash + 1);
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return Kind::other;
    const std::string ext = util::to_lower(name.substr(dot + 1));
    if (const auto it = extensions().find(ext); it != extensions().end()) return it->second;
    return split_part(ext) ? Kind::archive : Kind::other;
}

std::string_view name(Kind k) {
    switch (k) {
    case Kind::video: return "video";
    case Kind::audio: return "audio";
    case Kind::image: return "image";
    case Kind::text: return "text";
    case Kind::document: return "document";
    case Kind::archive: return "archive";
    case Kind::program: return "program";
    case Kind::disc: return "disc";
    case Kind::other: break;
    }
    return "other";
}

std::optional<Kind> parse(std::string_view text) {
    const std::string lower = util::to_lower(util::trim(text));
    for (const auto k : all_kinds) {
        if (lower == name(k)) return k;
    }
    return std::nullopt;
}

std::string names() {
    std::string out;
    for (std::size_t i = 0; i < std::size(all_kinds); ++i) {
        if (i > 0) out += i + 1 == std::size(all_kinds) ? " and " : ", ";
        out += name(all_kinds[i]);
    }
    return out;
}

std::optional<std::uint64_t> parse_size(std::string_view text) {
    std::string s;
    for (const char c : util::trim(text)) {
        if (c != ' ') s += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    }
    if (s.empty()) return std::nullopt;
    double unit = 1024.0 * 1024.0; // a bare number is MiB
    if (s.ends_with("ib")) {
        s.resize(s.size() - 2);
        if (s.empty() || std::string_view("kmgt").find(s.back()) == std::string_view::npos) return std::nullopt;
    } else if (s.ends_with('b')) {
        s.pop_back();
        if (s.empty()) return std::nullopt;
        if (std::string_view("kmgt").find(s.back()) == std::string_view::npos) unit = 1; // bytes: "500b"
    }
    if (!s.empty()) {
        switch (s.back()) {
        case 'k': unit = 1024.0; s.pop_back(); break;
        case 'm': unit = 1024.0 * 1024.0; s.pop_back(); break;
        case 'g': unit = 1024.0 * 1024.0 * 1024.0; s.pop_back(); break;
        case 't': unit = 1024.0 * 1024.0 * 1024.0 * 1024.0; s.pop_back(); break;
        default: break;
        }
    }
    double value = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (s.empty() || ec != std::errc{} || ptr != s.data() + s.size() || !std::isfinite(value) || value < 0) {
        return std::nullopt;
    }
    const double bytes = value * unit;
    if (bytes > 1e18) return std::nullopt;
    return static_cast<std::uint64_t>(std::llround(bytes));
}

bool Filter::matches(std::string_view file_name, std::optional<std::uint64_t> size) const {
    if (!kinds.empty() && std::ranges::find(kinds, kind_of(file_name)) == kinds.end()) return false;
    if (min_size || max_size) {
        if (!size) return false;
        if (min_size && *size < *min_size) return false;
        if (max_size && *size > *max_size) return false;
    }
    return true;
}

std::string Filter::describe() const {
    std::string out;
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        if (i > 0) out += " or ";
        out += name(kinds[i]);
    }
    if (min_size) out += std::format("{}at least {}", out.empty() ? "" : ", ", util::format_size(*min_size));
    if (max_size) out += std::format("{}at most {}", out.empty() ? "" : ", ", util::format_size(*max_size));
    return out;
}

} // namespace grab::file_type
