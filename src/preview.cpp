#include "preview.hpp"

#include "conn_budget.hpp"
#include "rclone.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <system_error>
#include <utility>

namespace grab::preview {

namespace {

std::string extension(std::string_view name) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || name.find('/', dot) != std::string_view::npos) return {};
    return util::to_lower(name.substr(dot + 1));
}

bool one_of(const std::string& ext, std::initializer_list<std::string_view> list) {
    for (const auto e : list) {
        if (ext == e) return true;
    }
    return false;
}

void append_utf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// The upper halves of the DOS code page 437 and of Windows-1252, as Unicode.
constexpr std::array<char16_t, 128> cp437{
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};
constexpr std::array<char16_t, 32> cp1252_80{
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

// The length of `s`'s valid UTF-8, a sequence cut off at the very end (the text's first MiB)
// counting as valid; npos when it isn't UTF-8.
std::size_t utf8_length(std::string_view s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t n = 0;
        if (c < 0x80) n = 1;
        else if (c >= 0xC2 && c <= 0xDF) n = 2;
        else if (c >= 0xE0 && c <= 0xEF) n = 3;
        else if (c >= 0xF0 && c <= 0xF4) n = 4;
        else return std::string_view::npos;
        if (i + n > s.size()) {
            for (std::size_t k = i + 1; k < s.size(); ++k) {
                if ((static_cast<unsigned char>(s[k]) & 0xC0) != 0x80) return std::string_view::npos;
            }
            return i; // cut off
        }
        for (std::size_t k = 1; k < n; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return std::string_view::npos;
        }
        i += n;
    }
    return i;
}

std::string utf16_to_utf8(std::string_view bytes, bool big_endian) {
    std::string out;
    out.reserve(bytes.size());
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        const auto a = static_cast<unsigned char>(bytes[i]);
        const auto b = static_cast<unsigned char>(bytes[i + 1]);
        char32_t unit = big_endian ? (a << 8 | b) : (b << 8 | a);
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < bytes.size()) {
            const auto c = static_cast<unsigned char>(bytes[i + 2]);
            const auto d = static_cast<unsigned char>(bytes[i + 3]);
            const char32_t low = big_endian ? (c << 8 | d) : (d << 8 | c);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        append_utf8(out, unit);
    }
    return out;
}

} // namespace

Kind kind_of(std::string_view name) {
    const auto ext = extension(name);
    if (ext.empty()) return Kind::none;
    if (one_of(ext, {"mkv", "mp4", "m4v", "avi", "mov", "wmv", "flv", "webm", "ts", "m2ts", "mts", "mpg", "mpeg", "vob",
                     "ogv", "3gp", "divx", "rmvb"})) {
        return Kind::video;
    }
    if (one_of(ext, {"mp3", "flac", "m4a", "m4b", "aac", "ogg", "oga", "opus", "wav", "wma", "ape", "aiff", "aif", "alac",
                     "mka", "dts", "ac3", "wv"})) {
        return Kind::audio;
    }
    if (one_of(ext, {"jpg", "jpeg", "jfif", "png", "gif", "webp", "avif", "bmp", "ico", "svg"})) return Kind::image;
    if (one_of(ext, {"nfo", "txt", "srt", "ass", "ssa", "vtt", "log", "md", "ini", "cfg", "conf", "json", "xml", "csv",
                     "cue", "sfv", "m3u", "m3u8", "yml", "yaml", "toml", "diz"})) {
        return Kind::text;
    }
    if (ext == "pdf") return Kind::pdf;
    return Kind::none;
}

std::string content_type(std::string_view name) {
    const auto ext = extension(name);
    switch (kind_of(name)) {
    case Kind::image:
        if (ext == "jpg" || ext == "jpeg" || ext == "jfif") return "image/jpeg";
        if (ext == "svg") return "image/svg+xml";
        if (ext == "ico") return "image/x-icon";
        return "image/" + ext;
    case Kind::text:
        return "text/plain; charset=utf-8";
    case Kind::pdf:
        return "application/pdf";
    default:
        return "application/octet-stream";
    }
}

std::size_t max_bytes(Kind kind) {
    switch (kind) {
    case Kind::text:
        return max_text;
    case Kind::image:
        return max_image;
    case Kind::pdf:
        return max_pdf;
    default:
        return 0;
    }
}

std::vector<std::string> serve_argv(const engine::Context& ctx, std::string_view root, std::string_view token) {
    std::vector<std::string> argv{ctx.config.rclone,     "serve",   "http", remote_spec(ctx.settings.rclone_remote, root),
                                  "--addr",              "127.0.0.1:0",
                                  "--baseurl",           "/" + std::string(token),
                                  "--read-only",         "--log-level", "NOTICE"};
    if (ctx.config.rclone_config) argv.insert(argv.end(), {"--config", util::path_to_utf8(*ctx.config.rclone_config)});
    const auto flags = engine::without_console_progress(ctx.settings.common_flags);
    argv.insert(argv.end(), flags.begin(), flags.end());
    argv.insert(argv.end(), {"--sftp-concurrency", budget::is_storage_box(ctx.remote.host) ? "4" : "8"});
    return argv;
}

std::optional<int> served_port(std::string_view log) {
    constexpr std::string_view at = "http://127.0.0.1:";
    const auto p = log.find(at);
    if (p == std::string_view::npos) return std::nullopt;
    const auto digits = log.substr(p + at.size());
    int port = 0;
    const auto [end, err] = std::from_chars(digits.data(), digits.data() + digits.size(), port);
    if (err != std::errc{} || end == digits.data() || port <= 0 || port > 65535) return std::nullopt;
    return port;
}

Where where(std::string_view remote_path) {
    if (!remote_path.starts_with('/')) return {"", std::string(remote_path)};
    while (remote_path.starts_with('/')) remote_path.remove_prefix(1);
    return {"/", std::string(remote_path)};
}

std::string url_path(std::string_view path) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size());
    for (const char ch : path) {
        const auto c = static_cast<unsigned char>(ch);
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                           c == '.' || c == '_' || c == '~' || c == '/';
        if (plain) {
            out += ch;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

std::optional<std::string> url_decode(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '%') {
            out += text[i];
            continue;
        }
        if (i + 2 >= text.size()) return std::nullopt;
        unsigned v = 0;
        const auto [end, err] = std::from_chars(text.data() + i + 1, text.data() + i + 3, v, 16);
        if (err != std::errc{} || end != text.data() + i + 3) return std::nullopt;
        out += static_cast<char>(v);
        i += 2;
    }
    return out;
}

std::string text_utf8(std::string_view bytes, bool nfo) {
    if (bytes.starts_with("\xFF\xFE")) return utf16_to_utf8(bytes.substr(2), false);
    if (bytes.starts_with("\xFE\xFF")) return utf16_to_utf8(bytes.substr(2), true);
    if (bytes.starts_with("\xEF\xBB\xBF")) bytes.remove_prefix(3);
    if (!nfo) {
        if (const auto n = utf8_length(bytes); n != std::string_view::npos) return std::string(bytes.substr(0, n));
    }
    std::string out;
    out.reserve(bytes.size() + bytes.size() / 2);
    for (const char ch : bytes) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x80) out += ch;
        else if (nfo) append_utf8(out, cp437[c - 0x80]);
        else if (c < 0xA0) append_utf8(out, cp1252_80[c - 0x80]);
        else append_utf8(out, c);
    }
    return out;
}

namespace {

// An EBML variable-length number at `pos`: its value (the length marker removed for sizes, kept
// for ids) and how many bytes it takes. nullopt past the end or for a malformed one.
std::optional<std::pair<std::uint64_t, std::size_t>> ebml_vint(std::string_view b, std::size_t pos, bool id) {
    if (pos >= b.size()) return std::nullopt;
    const auto first = static_cast<unsigned char>(b[pos]);
    std::size_t length = 1;
    unsigned mask = 0x80;
    while (length <= 8 && (first & mask) == 0) {
        mask >>= 1;
        ++length;
    }
    if (length > 8 || pos + length > b.size()) return std::nullopt;
    std::uint64_t value = id ? first : (first & (mask - 1));
    for (std::size_t i = 1; i < length; ++i) value = (value << 8) | static_cast<unsigned char>(b[pos + i]);
    return std::pair{value, length};
}

struct EbmlElement {
    std::uint64_t id = 0;
    std::uint64_t size = 0;
    bool unknown_size = false;
    std::size_t body = 0; // where its contents start
};

std::optional<EbmlElement> ebml_element(std::string_view b, std::size_t pos) {
    const auto id = ebml_vint(b, pos, true);
    if (!id) return std::nullopt;
    const auto size = ebml_vint(b, pos + id->second, false);
    if (!size) return std::nullopt;
    const std::size_t length = size->second;
    const std::uint64_t all_ones = (std::uint64_t{1} << (7 * length)) - 1;
    return EbmlElement{id->first, size->first, size->first == all_ones, pos + id->second + length};
}

constexpr std::uint64_t ebml_header_id = 0x1A45DFA3;
constexpr std::uint64_t segment_id = 0x18538067;
constexpr std::uint64_t cluster_id = 0x1F43B675;

} // namespace

std::optional<std::uint64_t> mkv_first_cluster(std::string_view head) {
    const auto header = ebml_element(head, 0);
    if (!header || header->id != ebml_header_id || header->unknown_size) return std::nullopt;
    const auto segment = ebml_element(head, static_cast<std::size_t>(header->body + header->size));
    if (!segment || segment->id != segment_id) return std::nullopt;
    // The Segment's children, until the first cluster.
    std::uint64_t pos = segment->body;
    while (true) {
        if (pos >= head.size()) return pos; // beyond what was read: the cluster comes at or after it
        const auto e = ebml_element(head, static_cast<std::size_t>(pos));
        if (!e) return pos;
        if (e->id == cluster_id || e->unknown_size) return pos;
        pos = e->body + e->size;
    }
}

std::uint64_t warm_start(std::string_view head, std::uint64_t size) {
    std::uint64_t want = warm_margin;
    if (const auto cluster = mkv_first_cluster(head)) want = *cluster + warm_margin;
    return std::min(size, std::min(want, warm_max));
}

Requested requested_range(std::string_view value, std::uint64_t size) {
    value = util::trim(value);
    if (util::to_lower(value.substr(0, 6)) != "bytes=") return {};
    value.remove_prefix(6);
    if (value.find(',') != std::string_view::npos) return {}; // several ranges: the whole file
    const auto dash = value.find('-');
    if (dash == std::string_view::npos) return {};
    auto number = [](std::string_view t) -> std::optional<std::uint64_t> {
        t = util::trim(t);
        std::uint64_t n = 0;
        const auto [end, err] = std::from_chars(t.data(), t.data() + t.size(), n);
        if (t.empty() || err != std::errc{} || end != t.data() + t.size()) return std::nullopt;
        return n;
    };
    const auto first = number(value.substr(0, dash));
    const auto last = number(value.substr(dash + 1));
    if (!first && last) { // "-500": the last 500 bytes
        if (*last == 0 || size == 0) return {Requested::Kind::unsatisfiable};
        return {Requested::Kind::part, size - std::min(*last, size), size - 1};
    }
    if (!first) return {};
    if (*first >= size) return {Requested::Kind::unsatisfiable};
    if (last && *last < *first) return {};
    return {Requested::Kind::part, *first, last ? std::min(*last, size - 1) : size - 1};
}

} // namespace grab::preview
