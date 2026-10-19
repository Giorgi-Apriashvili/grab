#include "local_name.hpp"

#include "util.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <vector>

namespace grab::local {

namespace {

constexpr char32_t quote = U'‛'; // ‛

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

// One piece of a name: a character, or a byte that isn't UTF-8 (as Go's utf8.DecodeRune sees it,
// which rclone uses: an invalid sequence costs one byte).
struct Piece {
    char32_t rune = 0;
    int bad = -1; // the byte, when not UTF-8
};

std::vector<Piece> pieces(std::string_view s) {
    std::vector<Piece> out;
    std::size_t i = 0;
    auto cont = [&](std::size_t k) { return k < s.size() && (static_cast<unsigned char>(s[k]) & 0xC0) == 0x80; };
    auto at = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    while (i < s.size()) {
        const unsigned char c = at(i);
        std::size_t n = 0;
        char32_t r = 0;
        if (c < 0x80) {
            n = 1;
            r = c;
        } else if (c >= 0xC2 && c <= 0xDF && cont(i + 1)) {
            n = 2;
            r = (c & 0x1F) << 6 | (at(i + 1) & 0x3F);
        } else if (c >= 0xE0 && c <= 0xEF && cont(i + 1) && cont(i + 2)) {
            const unsigned char d = at(i + 1);
            const bool ok = (c == 0xE0 && d >= 0xA0) || (c == 0xED && d <= 0x9F) || (c != 0xE0 && c != 0xED);
            if (ok) {
                n = 3;
                r = (c & 0x0F) << 12 | (d & 0x3F) << 6 | (at(i + 2) & 0x3F);
            }
        } else if (c >= 0xF0 && c <= 0xF4 && cont(i + 1) && cont(i + 2) && cont(i + 3)) {
            const unsigned char d = at(i + 1);
            const bool ok = (c == 0xF0 && d >= 0x90) || (c == 0xF4 && d <= 0x8F) || (c != 0xF0 && c != 0xF4);
            if (ok) {
                n = 4;
                r = (c & 0x07) << 18 | (d & 0x3F) << 12 | (at(i + 2) & 0x3F) << 6 | (at(i + 3) & 0x3F);
            }
        }
        if (n == 0) {
            out.push_back(Piece{0, c});
            ++i;
        } else {
            out.push_back(Piece{r, -1});
            i += n;
        }
    }
    return out;
}

// What rclone writes for a character that Windows won't take in a name (0 for one it keeps).
char32_t replacement(char32_t r) {
    if (r < 0x20) return U'␀' + r; // control characters: ␀..␟
    switch (r) {
    case U'"': return U'＂';
    case U'*': return U'＊';
    case U'/': return U'／';
    case U':': return U'：';
    case U'<': return U'＜';
    case U'>': return U'＞';
    case U'?': return U'？';
    case U'\\': return U'＼';
    case U'|': return U'｜';
    default: return 0;
    }
}

// A character that already is one of those replacements: quoted, so the name stays distinct.
bool quoted(char32_t r) {
    switch (r) {
    case U'＂': case U'＊': case U'／': case U'：': case U'＜': case U'＞':
    case U'？': case U'＼': case U'｜': case quote:
        return true;
    default:
        return false;
    }
}

char32_t upper(char32_t c) { return c >= U'a' && c <= U'z' ? c - 32 : c; }

} // namespace

bool is_device(std::string_view local_name) {
    // The part before the first period, trailing spaces aside, as Windows compares it.
    auto base = local_name.substr(0, local_name.find('.'));
    while (base.ends_with(' ')) base.remove_suffix(1);
    std::u32string b;
    for (const auto& p : pieces(base)) b += upper(p.rune);
    if (b == U"CON" || b == U"PRN" || b == U"AUX" || b == U"NUL" || b == U"CONIN$" || b == U"CONOUT$") return true;
    if (b.size() == 4 && (b.starts_with(U"COM") || b.starts_with(U"LPT"))) {
        const char32_t d = b[3];
        return (d >= U'0' && d <= U'9') || d == U'¹' || d == U'²' || d == U'³';
    }
    return false;
}

std::string name(std::string_view remote_name) {
    if (remote_name == ".") return "．";
    if (remote_name == "..") return "．．";
    const auto ps = pieces(remote_name);
    std::string out;
    out.reserve(remote_name.size() + 8);
    for (std::size_t i = 0; i < ps.size(); ++i) {
        const auto& p = ps[i];
        if (p.bad >= 0) {
            append_utf8(out, quote);
            out += std::format("{:02X}", p.bad);
            continue;
        }
        const bool last = i + 1 == ps.size();
        if (last && p.rune == U' ') {
            append_utf8(out, U'␠'); // ␠
        } else if (last && p.rune == U'.') {
            append_utf8(out, U'．'); // ．
        } else if (last && (p.rune == U'␠' || p.rune == U'．')) {
            append_utf8(out, quote);
            append_utf8(out, p.rune);
        } else if (const char32_t rep = replacement(p.rune)) {
            append_utf8(out, rep);
        } else if (quoted(p.rune)) {
            append_utf8(out, quote);
            append_utf8(out, p.rune);
        } else {
            append_utf8(out, p.rune);
        }
    }
    if (is_device(out)) out.insert(std::min(out.find('.'), out.size()), "_");
    return out;
}

bool has_device(std::string_view remote_rel) {
    while (!remote_rel.empty()) {
        const auto slash = remote_rel.find('/');
        const auto part = remote_rel.substr(0, slash);
        if (!part.empty() && is_device(part)) return true;
        if (slash == std::string_view::npos) break;
        remote_rel.remove_prefix(slash + 1);
    }
    return false;
}

std::filesystem::path relative(std::string_view remote_rel) {
    std::filesystem::path out;
    while (!remote_rel.empty()) {
        const auto slash = remote_rel.find('/');
        if (const auto part = remote_rel.substr(0, slash); !part.empty()) out /= util::path_from_utf8(name(part));
        if (slash == std::string_view::npos) break;
        remote_rel.remove_prefix(slash + 1);
    }
    return out;
}

} // namespace grab::local
