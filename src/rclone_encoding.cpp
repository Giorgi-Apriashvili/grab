#include "rclone_encoding.hpp"

#include "util.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
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

// A range of letters and what Go's unicode.ToUpper adds to each: Go's CaseRange with only its
// UpperCase delta. upper_lower is Go's UpperLower: upper- and lower-case letters by turns, an
// upper-case one at every even offset from `lo`.
struct CaseRange {
    constexpr CaseRange(char32_t first, char32_t last, std::int32_t add) : lo(first), hi(last), delta(add) {}
    char32_t lo;
    char32_t hi;
    std::int32_t delta;
};
constexpr std::int32_t upper_lower = 0x110000; // Go's MaxRune + 1

// Go 1.26.8's unicode.CaseRanges (src/unicode/tables.go, Unicode 15.0.0; Go 1.26 is what rclone
// 1.75.1 is built with), the 203 of its 328 ranges whose UpperCase delta isn't 0, in its order.
// Generated from that file; a letter in none of them is its own upper case.
constexpr auto case_ranges = std::to_array<CaseRange>({
    {0x0061, 0x007A, -32}, {0x00B5, 0x00B5, 743}, {0x00E0, 0x00F6, -32}, {0x00F8, 0x00FE, -32},
    {0x00FF, 0x00FF, 121}, {0x0100, 0x012F, upper_lower}, {0x0131, 0x0131, -232}, {0x0132, 0x0137, upper_lower},
    {0x0139, 0x0148, upper_lower}, {0x014A, 0x0177, upper_lower}, {0x0179, 0x017E, upper_lower},
    {0x017F, 0x017F, -300}, {0x0180, 0x0180, 195}, {0x0182, 0x0185, upper_lower}, {0x0187, 0x0188, upper_lower},
    {0x018B, 0x018C, upper_lower}, {0x0191, 0x0192, upper_lower}, {0x0195, 0x0195, 97},
    {0x0198, 0x0199, upper_lower}, {0x019A, 0x019A, 163}, {0x019E, 0x019E, 130}, {0x01A0, 0x01A5, upper_lower},
    {0x01A7, 0x01A8, upper_lower}, {0x01AC, 0x01AD, upper_lower}, {0x01AF, 0x01B0, upper_lower},
    {0x01B3, 0x01B6, upper_lower}, {0x01B8, 0x01B9, upper_lower}, {0x01BC, 0x01BD, upper_lower},
    {0x01BF, 0x01BF, 56}, {0x01C5, 0x01C5, -1}, {0x01C6, 0x01C6, -2}, {0x01C8, 0x01C8, -1}, {0x01C9, 0x01C9, -2},
    {0x01CB, 0x01CB, -1}, {0x01CC, 0x01CC, -2}, {0x01CD, 0x01DC, upper_lower}, {0x01DD, 0x01DD, -79},
    {0x01DE, 0x01EF, upper_lower}, {0x01F2, 0x01F2, -1}, {0x01F3, 0x01F3, -2}, {0x01F4, 0x01F5, upper_lower},
    {0x01F8, 0x021F, upper_lower}, {0x0222, 0x0233, upper_lower}, {0x023B, 0x023C, upper_lower},
    {0x023F, 0x0240, 10815}, {0x0241, 0x0242, upper_lower}, {0x0246, 0x024F, upper_lower}, {0x0250, 0x0250, 10783},
    {0x0251, 0x0251, 10780}, {0x0252, 0x0252, 10782}, {0x0253, 0x0253, -210}, {0x0254, 0x0254, -206},
    {0x0256, 0x0257, -205}, {0x0259, 0x0259, -202}, {0x025B, 0x025B, -203}, {0x025C, 0x025C, 42319},
    {0x0260, 0x0260, -205}, {0x0261, 0x0261, 42315}, {0x0263, 0x0263, -207}, {0x0265, 0x0265, 42280},
    {0x0266, 0x0266, 42308}, {0x0268, 0x0268, -209}, {0x0269, 0x0269, -211}, {0x026A, 0x026A, 42308},
    {0x026B, 0x026B, 10743}, {0x026C, 0x026C, 42305}, {0x026F, 0x026F, -211}, {0x0271, 0x0271, 10749},
    {0x0272, 0x0272, -213}, {0x0275, 0x0275, -214}, {0x027D, 0x027D, 10727}, {0x0280, 0x0280, -218},
    {0x0282, 0x0282, 42307}, {0x0283, 0x0283, -218}, {0x0287, 0x0287, 42282}, {0x0288, 0x0288, -218},
    {0x0289, 0x0289, -69}, {0x028A, 0x028B, -217}, {0x028C, 0x028C, -71}, {0x0292, 0x0292, -219},
    {0x029D, 0x029D, 42261}, {0x029E, 0x029E, 42258}, {0x0345, 0x0345, 84}, {0x0370, 0x0373, upper_lower},
    {0x0376, 0x0377, upper_lower}, {0x037B, 0x037D, 130}, {0x03AC, 0x03AC, -38}, {0x03AD, 0x03AF, -37},
    {0x03B1, 0x03C1, -32}, {0x03C2, 0x03C2, -31}, {0x03C3, 0x03CB, -32}, {0x03CC, 0x03CC, -64},
    {0x03CD, 0x03CE, -63}, {0x03D0, 0x03D0, -62}, {0x03D1, 0x03D1, -57}, {0x03D5, 0x03D5, -47},
    {0x03D6, 0x03D6, -54}, {0x03D7, 0x03D7, -8}, {0x03D8, 0x03EF, upper_lower}, {0x03F0, 0x03F0, -86},
    {0x03F1, 0x03F1, -80}, {0x03F2, 0x03F2, 7}, {0x03F3, 0x03F3, -116}, {0x03F5, 0x03F5, -96},
    {0x03F7, 0x03F8, upper_lower}, {0x03FA, 0x03FB, upper_lower}, {0x0430, 0x044F, -32}, {0x0450, 0x045F, -80},
    {0x0460, 0x0481, upper_lower}, {0x048A, 0x04BF, upper_lower}, {0x04C1, 0x04CE, upper_lower},
    {0x04CF, 0x04CF, -15}, {0x04D0, 0x052F, upper_lower}, {0x0561, 0x0586, -48}, {0x10D0, 0x10FA, 3008},
    {0x10FD, 0x10FF, 3008}, {0x13F8, 0x13FD, -8}, {0x1C80, 0x1C80, -6254}, {0x1C81, 0x1C81, -6253},
    {0x1C82, 0x1C82, -6244}, {0x1C83, 0x1C84, -6242}, {0x1C85, 0x1C85, -6243}, {0x1C86, 0x1C86, -6236},
    {0x1C87, 0x1C87, -6181}, {0x1C88, 0x1C88, 35266}, {0x1D79, 0x1D79, 35332}, {0x1D7D, 0x1D7D, 3814},
    {0x1D8E, 0x1D8E, 35384}, {0x1E00, 0x1E95, upper_lower}, {0x1E9B, 0x1E9B, -59}, {0x1EA0, 0x1EFF, upper_lower},
    {0x1F00, 0x1F07, 8}, {0x1F10, 0x1F15, 8}, {0x1F20, 0x1F27, 8}, {0x1F30, 0x1F37, 8}, {0x1F40, 0x1F45, 8},
    {0x1F51, 0x1F51, 8}, {0x1F53, 0x1F53, 8}, {0x1F55, 0x1F55, 8}, {0x1F57, 0x1F57, 8}, {0x1F60, 0x1F67, 8},
    {0x1F70, 0x1F71, 74}, {0x1F72, 0x1F75, 86}, {0x1F76, 0x1F77, 100}, {0x1F78, 0x1F79, 128}, {0x1F7A, 0x1F7B, 112},
    {0x1F7C, 0x1F7D, 126}, {0x1F80, 0x1F87, 8}, {0x1F90, 0x1F97, 8}, {0x1FA0, 0x1FA7, 8}, {0x1FB0, 0x1FB1, 8},
    {0x1FB3, 0x1FB3, 9}, {0x1FBE, 0x1FBE, -7205}, {0x1FC3, 0x1FC3, 9}, {0x1FD0, 0x1FD1, 8}, {0x1FE0, 0x1FE1, 8},
    {0x1FE5, 0x1FE5, 7}, {0x1FF3, 0x1FF3, 9}, {0x214E, 0x214E, -28}, {0x2170, 0x217F, -16},
    {0x2183, 0x2184, upper_lower}, {0x24D0, 0x24E9, -26}, {0x2C30, 0x2C5F, -48}, {0x2C60, 0x2C61, upper_lower},
    {0x2C65, 0x2C65, -10795}, {0x2C66, 0x2C66, -10792}, {0x2C67, 0x2C6C, upper_lower}, {0x2C72, 0x2C73, upper_lower},
    {0x2C75, 0x2C76, upper_lower}, {0x2C80, 0x2CE3, upper_lower}, {0x2CEB, 0x2CEE, upper_lower},
    {0x2CF2, 0x2CF3, upper_lower}, {0x2D00, 0x2D25, -7264}, {0x2D27, 0x2D27, -7264}, {0x2D2D, 0x2D2D, -7264},
    {0xA640, 0xA66D, upper_lower}, {0xA680, 0xA69B, upper_lower}, {0xA722, 0xA72F, upper_lower},
    {0xA732, 0xA76F, upper_lower}, {0xA779, 0xA77C, upper_lower}, {0xA77E, 0xA787, upper_lower},
    {0xA78B, 0xA78C, upper_lower}, {0xA790, 0xA793, upper_lower}, {0xA794, 0xA794, 48},
    {0xA796, 0xA7A9, upper_lower}, {0xA7B4, 0xA7C3, upper_lower}, {0xA7C7, 0xA7CA, upper_lower},
    {0xA7D0, 0xA7D1, upper_lower}, {0xA7D6, 0xA7D9, upper_lower}, {0xA7F5, 0xA7F6, upper_lower},
    {0xAB53, 0xAB53, -928}, {0xAB70, 0xABBF, -38864}, {0xFF41, 0xFF5A, -32}, {0x10428, 0x1044F, -40},
    {0x104D8, 0x104FB, -40}, {0x10597, 0x105A1, -39}, {0x105A3, 0x105B1, -39}, {0x105B3, 0x105B9, -39},
    {0x105BB, 0x105BC, -39}, {0x10CC0, 0x10CF2, -64}, {0x118C0, 0x118DF, -32}, {0x16E60, 0x16E7F, -32},
    {0x1E922, 0x1E943, -34}
});

// Go's unicode.ToUpper.
char32_t go_upper_rune(char32_t r) {
    if (r < 0x80) return r >= U'a' && r <= U'z' ? r - (U'a' - U'A') : r;
    const auto it = std::ranges::lower_bound(case_ranges, r, {}, &CaseRange::hi);
    if (it == case_ranges.end() || r < it->lo) return r;
    if (it->delta == upper_lower) return it->lo + ((r - it->lo) & ~char32_t{1});
    return static_cast<char32_t>(static_cast<std::int32_t>(r) + it->delta);
}

} // namespace

bool valid_utf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        const Rune r = rune_at(s, i);
        if (r.bad) return false;
        i += r.len;
    }
    return true;
}

std::string go_upper(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const Rune c = rune_at(s, i); // a byte that isn't UTF-8 is U+FFFD, as Go's range loop reads it
        put(out, go_upper_rune(c.r));
        i += c.len;
    }
    return out;
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

std::string from_raw_path(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t start = 0;;) {
        const std::size_t sep = raw.find('/', start);
        out += from_raw_name(raw.substr(start, sep == std::string_view::npos ? std::string_view::npos : sep - start));
        if (sep == std::string_view::npos) break;
        out += '/';
        start = sep + 1;
    }
    return out;
}

std::string shown_line(std::string_view text, std::size_t max) {
    while (!text.empty()) {
        const auto nl = text.find('\n');
        const auto line = util::trim(text.substr(0, nl));
        if (!line.empty()) {
            std::string shown = encode(line.substr(0, max), ctl | del | invalid_utf8);
            return line.size() > max ? shown + "…" : shown;
        }
        if (nl == std::string_view::npos) break;
        text.remove_prefix(nl + 1);
    }
    return {};
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
