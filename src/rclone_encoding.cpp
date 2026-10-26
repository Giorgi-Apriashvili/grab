#include "rclone_encoding.hpp"

#include "util.hpp"

#include <cstddef>
#include <format>

namespace grab::rclone_enc {

namespace {

constexpr char32_t quote = U'\u201B';  // ‛ QuoteRune
constexpr char32_t symbol = U'\u2400'; // ␀ SYMBOL FOR NULL: + 0x01..0x1F gives ␁..␟
constexpr char32_t full = 0xFEE0;      // ASCII + this = its FULLWIDTH form
constexpr char32_t rune_error = 0xFFFD;

// A character of a name as Go's utf8.DecodeRuneInString reads it: an invalid sequence is one
// byte of utf8.RuneError.
struct Rune {
    char32_t r = 0;
    std::size_t len = 0;
    bool bad = false;
};

Rune rune_at(std::string_view s, std::size_t i) {
    const auto at = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    const auto cont = [&](std::size_t k) { return k < s.size() && (at(k) & 0xC0) == 0x80; };
    const unsigned char c = at(i);
    if (c < 0x80) return {c, 1, false};
    if (c >= 0xC2 && c <= 0xDF && cont(i + 1)) return {static_cast<char32_t>((c & 0x1F) << 6 | (at(i + 1) & 0x3F)), 2, false};
    if (c >= 0xE0 && c <= 0xEF && cont(i + 1) && cont(i + 2)) {
        const unsigned char d = at(i + 1);
        if ((c == 0xE0 && d >= 0xA0) || (c == 0xED && d <= 0x9F) || (c != 0xE0 && c != 0xED)) {
            return {static_cast<char32_t>((c & 0x0F) << 12 | (d & 0x3F) << 6 | (at(i + 2) & 0x3F)), 3, false};
        }
    }
    if (c >= 0xF0 && c <= 0xF4 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
        const unsigned char d = at(i + 1);
        if ((c == 0xF0 && d >= 0x90) || (c == 0xF4 && d <= 0x8F) || (c != 0xF0 && c != 0xF4)) {
            return {static_cast<char32_t>((c & 0x07) << 18 | (d & 0x3F) << 12 | (at(i + 2) & 0x3F) << 6 | (at(i + 3) & 0x3F)),
                    4, false};
        }
    }
    return {rune_error, 1, true};
}

// Go's utf8.DecodeLastRuneInString.
Rune last_rune(std::string_view s) {
    if (s.empty()) return {rune_error, 0, true};
    const std::size_t end = s.size();
    if (static_cast<unsigned char>(s[end - 1]) < 0x80) return {static_cast<unsigned char>(s[end - 1]), 1, false};
    const std::size_t lim = end >= 4 ? end - 4 : 0;
    std::size_t start = end - 1;
    while (start > lim && (static_cast<unsigned char>(s[start]) & 0xC0) == 0x80) --start;
    const Rune r = rune_at(s, start);
    if (r.bad || start + r.len != end) return {rune_error, 1, true};
    return r;
}

void put(std::string& out, char32_t c) {
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

std::string utf8(char32_t c) {
    std::string s;
    put(s, c);
    return s;
}

// The ASCII characters a flag replaces with their FULLWIDTH forms.
bool fullwidth_ascii(char32_t r, Mask mask) {
    switch (r) {
    case U'*': return (mask & asterisk) != 0;
    case U'<': case U'>': return (mask & lt_gt) != 0;
    case U'?': return (mask & question) != 0;
    case U':': return (mask & colon) != 0;
    case U'|': return (mask & pipe) != 0;
    case U'"': return (mask & double_quote) != 0;
    case U'/': return (mask & slash) != 0;
    case U'\\': return (mask & back_slash) != 0;
    default: return false;
    }
}

bool is_ctl_symbol(char32_t r) { return r > symbol && r <= symbol + 0x1F; }

// A character Encode changes (as rclone's IndexFunc finds the first one).
bool encode_touches(const Rune& c, Mask mask) {
    const char32_t r = c.r;
    if (r == 0 || r == symbol || r == quote || r == rune_error) return true;
    if (fullwidth_ascii(r, mask) || (r >= full + 0x21 && r <= full + 0x7E && fullwidth_ascii(r - full, mask))) return true;
    if ((mask & del) != 0 && (r == 0x7F || r == U'\u2421')) return true;
    if ((mask & ctl) != 0 && ((r >= 1 && r <= 0x1F) || is_ctl_symbol(r))) return true;
    return false;
}

// A character Decode changes.
bool decode_touches(char32_t r, Mask mask) {
    if (r == symbol || r == quote) return true;
    if (r >= full + 0x21 && r <= full + 0x7E && fullwidth_ascii(r - full, mask)) return true;
    if ((mask & del) != 0 && r == U'\u2421') return true;
    if ((mask & ctl) != 0 && is_ctl_symbol(r)) return true;
    return false;
}

bool hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

} // namespace

bool valid_utf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const Rune r = rune_at(s, i);
        if (r.bad) return false;
        i += r.len;
    }
    return true;
}

std::string encode(std::string_view in, Mask mask) {
    if (in.empty()) return {};
    if ((mask & dot) != 0) {
        if (in == ".") return utf8(U'\uFF0E');
        if (in == "..") return utf8(U'\uFF0E') + utf8(U'\uFF0E');
        if (in == utf8(U'\uFF0E')) return utf8(quote) + utf8(U'\uFF0E');
        if (in == utf8(U'\uFF0E') + utf8(U'\uFF0E')) return utf8(quote) + utf8(U'\uFF0E') + utf8(quote) + utf8(U'\uFF0E');
    }
    std::string suffix;
    if ((mask & right_space) != 0) {
        if (in.back() == ' ') {
            suffix = utf8(U'\u2420'); // ␠
            in.remove_suffix(1);
        } else if (const Rune last = last_rune(in); !last.bad && last.r == U'\u2420') {
            suffix = utf8(quote) + utf8(U'\u2420');
            in.remove_suffix(last.len);
        }
    }
    if ((mask & right_period) != 0 && suffix.empty() && !in.empty()) {
        if (in.back() == '.') {
            suffix = utf8(U'\uFF0E'); // ．
            in.remove_suffix(1);
        } else if (const Rune last = last_rune(in); !last.bad && last.r == U'\uFF0E') {
            suffix = utf8(quote) + utf8(U'\uFF0E');
            in.remove_suffix(last.len);
        }
    }

    std::size_t index = 0;
    if (suffix.empty()) {
        index = in.size();
        for (std::size_t i = 0; i < in.size();) {
            const Rune c = rune_at(in, i);
            if (encode_touches(c, mask)) {
                index = i;
                break;
            }
            i += c.len;
        }
        if (index == in.size()) return std::string(in);
    }

    std::string out(in.substr(0, index));
    for (std::size_t i = index; i < in.size();) {
        const Rune c = rune_at(in, i);
        const char32_t r = c.r;
        const std::size_t at = i;
        i += c.len;
        if (r == 0) {
            put(out, symbol);
            continue;
        }
        if (r == symbol || r == quote) {
            put(out, quote);
            put(out, r);
            continue;
        }
        if (r == rune_error) {
            if ((mask & invalid_utf8) != 0) {
                if (c.bad) { // a literal U+FFFD is kept as it is
                    out += std::format("{}{:02X}", utf8(quote), static_cast<unsigned char>(in[at]));
                    continue;
                }
            } else {
                out += in.substr(at, c.len);
                continue;
            }
        }
        if (fullwidth_ascii(r, mask)) {
            put(out, r + full);
            continue;
        }
        if (r >= full + 0x21 && r <= full + 0x7E && fullwidth_ascii(r - full, mask)) {
            put(out, quote);
            put(out, r);
            continue;
        }
        if ((mask & del) != 0) {
            if (r == 0x7F) {
                put(out, U'\u2421'); // ␡
                continue;
            }
            if (r == U'\u2421') {
                put(out, quote);
                put(out, r);
                continue;
            }
        }
        if ((mask & ctl) != 0) {
            if (r >= 1 && r <= 0x1F) {
                put(out, symbol + r);
                continue;
            }
            if (is_ctl_symbol(r)) {
                put(out, quote);
                put(out, r);
                continue;
            }
        }
        out += in.substr(at, c.len);
    }
    return out + suffix;
}

std::string decode(std::string_view in, Mask mask) {
    if ((mask & dot) != 0) {
        const auto period = utf8(U'\uFF0E');
        if (in == period) return ".";
        if (in == period + period) return "..";
        if (in == utf8(quote) + period) return period;
        if (in == utf8(quote) + period + utf8(quote) + period) return period + period;
    }
    std::string suffix;
    const Rune last = last_rune(in);
    if ((mask & right_space) != 0 && !last.bad && last.r == U'\u2420') {
        in.remove_suffix(last.len);
        if (const Rune q = last_rune(in); !q.bad && q.r == quote) {
            suffix = utf8(U'\u2420');
            in.remove_suffix(q.len);
        } else {
            suffix = " ";
        }
    } else if ((mask & right_period) != 0 && !last.bad && last.r == U'\uFF0E') {
        in.remove_suffix(last.len);
        if (const Rune q = last_rune(in); !q.bad && q.r == quote) {
            suffix = utf8(U'\uFF0E');
            in.remove_suffix(q.len);
        } else {
            suffix = ".";
        }
    }

    std::size_t index = 0;
    if (suffix.empty()) {
        index = in.size();
        for (std::size_t i = 0; i < in.size();) {
            const Rune c = rune_at(in, i);
            if (!c.bad && decode_touches(c.r, mask)) {
                index = i;
                break;
            }
            i += c.len;
        }
        if (index == in.size()) return std::string(in);
    }

    std::string out(in.substr(0, index));
    bool unquote_next = false;
    bool skip_next = false;
    for (std::size_t i = index; i < in.size();) {
        const Rune c = rune_at(in, i);
        const char32_t r = c.r;
        const std::size_t at = i;
        i += c.len;
        if (skip_next) {
            skip_next = false;
            continue;
        }
        const bool unquote = unquote_next;
        unquote_next = false;
        if (!c.bad && r == symbol) {
            if (unquote) put(out, r);
            else out += '\0';
            continue;
        }
        if (!c.bad && r == quote) {
            if (unquote) put(out, r);
            else unquote_next = true;
            continue;
        }
        if (!c.bad && r >= full + 0x21 && r <= full + 0x7E && fullwidth_ascii(r - full, mask)) {
            put(out, unquote ? r : r - full);
            continue;
        }
        if ((mask & del) != 0 && !c.bad && r == U'\u2421') {
            put(out, unquote ? r : char32_t{0x7F});
            continue;
        }
        if ((mask & ctl) != 0 && !c.bad && is_ctl_symbol(r)) {
            put(out, unquote ? r : r - symbol);
            continue;
        }
        if (unquote) {
            // ‛XX: a byte that wasn't UTF-8 (rclone's appendUnquotedByte reads the next two bytes)
            if ((mask & invalid_utf8) != 0 && at + 2 <= in.size() && hex(in[at]) && hex(in[at + 1])) {
                out += static_cast<char>(std::stoi(std::string(in.substr(at, 2)), nullptr, 16));
                skip_next = true;
                continue;
            }
            put(out, quote);
        }
        out += in.substr(at, c.len);
    }
    if (unquote_next) put(out, quote);
    return out + suffix;
}

std::string from_raw_name(std::string_view raw) { return encode(raw, grab_path); }

std::string raw_path(std::string_view path) {
    std::string out;
    out.reserve(path.size());
    for (std::size_t start = 0;;) {
        const std::size_t sep = path.find('/', start);
        out += decode(path.substr(start, sep == std::string_view::npos ? std::string_view::npos : sep - start), grab_path);
        if (sep == std::string_view::npos) break;
        out += '/';
        start = sep + 1;
    }
    return out;
}

std::string from_standard_name(std::string_view name) { return encode(decode(name, standard), windows); }

std::string to_standard_name(std::string_view name) { return encode(decode(name, windows), standard); }

std::expected<std::string, std::string> local_path(const std::filesystem::path& p) {
#ifdef _WIN32
    // rclone's cleanRootPath encodes every part after the volume ("D:", "\\server\share"); "."
    // and ".." stay.
    const auto root_name = p.root_name().native();
    const bool share = root_name.starts_with(L"\\\\") && !root_name.starts_with(L"\\\\?");
    std::string out = util::path_to_utf8(p.root_name()) + util::path_to_utf8(p.root_directory());
    bool first = true;
    for (const auto& part : p.relative_path()) {
        const std::string name = util::path_to_utf8(part);
        if (name.empty()) continue;
        std::string given = name;
        if (!(first && share) && name != "." && name != "..") {
            given = to_standard_name(name);
            // A "\" (from a server's "＼") would split the part in two; bytes that aren't UTF-8
            // can't be on a command line; and some names rclone can't write as they are ("a‛b").
            if (given.find('\\') != std::string::npos || !valid_utf8(given) || from_standard_name(given) != name) {
                return util::failf("rclone can't be given the folder or file name \"{}\" as it is; rename it or "
                                   "choose another folder",
                                   name);
            }
        }
        if (!first) out += '\\';
        out += given;
        first = false;
    }
    return out;
#else
    return util::path_to_utf8(p);
#endif
}

} // namespace grab::rclone_enc
