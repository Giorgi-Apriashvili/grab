#include "local_name.hpp"

#include "rclone_encoding.hpp"
#include "util.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)
#endif

namespace grab::local {

namespace {

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

// Bidi controls: marks, embeddings, overrides and isolates. They make a name read otherwise
// than it is ("invoice<U+202E>fdp.exe" shows as "invoiceexe.pdf"), in grab's window and in
// Explorer alike (review G51).
bool is_bidi(char32_t r) {
    return r == 0x061C || r == 0x200E || r == 0x200F || (r >= 0x202A && r <= 0x202E) || (r >= 0x2066 && r <= 0x2069);
}

std::string without_bidi(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const auto& p : pieces(s)) {
        if (p.bad >= 0) out += static_cast<char>(p.bad);
        else if (!is_bidi(p.rune)) append_utf8(out, p.rune);
    }
    return out;
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

bool is_short_name(std::string_view local_name) {
    // NAME~N or NAME~N.EXT as Windows makes them: up to 8 ASCII characters ending in ~ and a
    // number, up to 3 after a single period. Windows finds them in any case.
    const auto dot = local_name.find('.');
    const auto base = local_name.substr(0, dot);
    if (dot != std::string_view::npos) {
        const auto ext = local_name.substr(dot + 1);
        if (ext.empty() || ext.size() > 3 || ext.find('.') != std::string_view::npos) return false;
    }
    if (base.size() < 3 || base.size() > 8) return false;
    if (!std::ranges::all_of(local_name, [](char c) { return static_cast<unsigned char>(c) < 0x80; })) return false;
    const auto tilde = base.rfind('~');
    if (tilde == std::string_view::npos || tilde == 0 || tilde + 1 == base.size()) return false;
    return std::ranges::all_of(base.substr(tilde + 1), [](char c) { return c >= '0' && c <= '9'; });
}

std::string name(std::string_view remote_name) {
    // rclone's mapping (FromStandardName), the name read in grab's form: a byte that isn't UTF-8
    // may come as ‛XX (rclone_enc::grab_path) or raw, and lands the same either way.
    std::string out = rclone_enc::encode(rclone_enc::decode(without_bidi(remote_name), rclone_enc::grab_path),
                                         rclone_enc::windows);
    if (out.empty()) return "_"; // nothing but bidi controls
    if (is_device(out) || is_short_name(out)) out.insert(std::min(out.find('.'), out.size()), "_");
    return out;
}

bool differs_from_rclone(std::string_view remote_rel) {
    while (!remote_rel.empty()) {
        const auto slash = remote_rel.find('/');
        const auto part = remote_rel.substr(0, slash);
        if (!part.empty() && name(part) != rclone_enc::from_standard_name(part)) return true;
        if (slash == std::string_view::npos) break;
        remote_rel.remove_prefix(slash + 1);
    }
    return false;
}

std::string bidi_visible(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const auto& p : pieces(text)) {
        if (p.bad >= 0) out += static_cast<char>(p.bad);
        else if (is_bidi(p.rune)) out += std::format("⟨U+{:04X}⟩", static_cast<std::uint32_t>(p.rune));
        else append_utf8(out, p.rune);
    }
    return out;
}

std::filesystem::path::string_type key(const std::filesystem::path& p) {
    auto s = p.native();
#ifdef _WIN32
    // As NTFS does: upper case, the same in every locale.
    if (!s.empty()) {
        std::wstring up(s.size(), L'\0');
        const int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, s.data(), static_cast<int>(s.size()),
                                    up.data(), static_cast<int>(up.size()), nullptr, nullptr, 0);
        if (n > 0) {
            up.resize(static_cast<std::size_t>(n));
            return up;
        }
    }
#endif
    return s;
}

std::filesystem::path::string_type fold(const std::filesystem::path& p) {
    auto k = key(p);
#ifdef _WIN32
    if (k.empty()) return k;
    const int estimate = NormalizeString(NormalizationC, k.data(), static_cast<int>(k.size()), nullptr, 0);
    if (estimate > 0) {
        std::wstring out(static_cast<std::size_t>(estimate), L'\0');
        const int n = NormalizeString(NormalizationC, k.data(), static_cast<int>(k.size()), out.data(), estimate);
        if (n > 0) {
            out.resize(static_cast<std::size_t>(n));
            return out;
        }
    }
#endif
    return k;
}

std::unordered_set<std::string> clashing(std::span<const std::string> remote_rels) {
    // Each spelling of every folder and file on the way, by how rclone matches it.
    std::unordered_map<std::filesystem::path::string_type, std::unordered_set<std::filesystem::path::string_type>> spellings;
    std::vector<std::filesystem::path> locals;
    locals.reserve(remote_rels.size());
    for (const auto& r : remote_rels) {
        locals.push_back(relative(r));
        std::filesystem::path prefix;
        for (const auto& part : locals.back()) {
            prefix /= part;
            spellings[fold(prefix)].insert(prefix.native());
        }
    }
    std::unordered_set<std::filesystem::path::string_type> spelled_twice;
    for (const auto& [folded, spelled] : spellings) {
        if (spelled.size() > 1) spelled_twice.insert(spelled.begin(), spelled.end());
    }
    std::unordered_set<std::string> out;
    if (spelled_twice.empty()) return out;
    for (std::size_t i = 0; i < remote_rels.size(); ++i) {
        std::filesystem::path prefix;
        for (const auto& part : locals[i]) {
            prefix /= part;
            if (spelled_twice.contains(prefix.native())) {
                out.insert(remote_rels[i]);
                break;
            }
        }
    }
    return out;
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
