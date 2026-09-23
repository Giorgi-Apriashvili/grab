#include "match.hpp"

#include "util.hpp"

#include <algorithm>
#include <cstddef>

namespace grab {

namespace {

constexpr char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
constexpr char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

// The last component of a remote path ("a/b/c" -> "c").
std::string_view leaf(std::string_view path) {
    while (path.size() > 1 && path.ends_with('/')) path.remove_suffix(1);
    const auto slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

} // namespace

bool is_glob(std::string_view s) { return s.find_first_of("*?[") != std::string_view::npos; }

Query make_query(std::string_view target, bool exact) {
    Query q;
    q.text = std::string(target);
    if (exact) {
        q.kind = Query::Kind::exact;
        return q;
    }
    if (is_glob(target)) {
        q.kind = Query::Kind::glob;
        return q;
    }
    std::string word;
    for (const char c : target) {
        if (c == ' ' || c == '\t') {
            if (!word.empty()) q.terms.push_back(std::move(word));
            word.clear();
        } else {
            word += lower(c);
        }
    }
    if (!word.empty()) q.terms.push_back(std::move(word));
    q.kind = q.terms.empty() ? Query::Kind::exact : Query::Kind::words;
    return q;
}

bool matches(const Query& q, std::string_view name) {
    name = leaf(name);
    switch (q.kind) {
    case Query::Kind::exact: return glob_match(q.text, name, false);
    case Query::Kind::glob: return glob_match(q.text, name, true);
    case Query::Kind::words: {
        // Lowercase once, then a memchr-driven substring scan per word.
        const std::string folded = util::to_lower(name);
        return std::ranges::all_of(q.terms, [&](const std::string& t) {
            return std::string_view(folded).find(t) != std::string_view::npos;
        });
    }
    }
    return false;
}

int match_tier(const Query& q, std::string_view name) {
    const std::string folded = util::to_lower(leaf(name));
    if (folded == util::to_lower(q.text)) return 0;
    if (q.kind == Query::Kind::words && !q.terms.empty() && folded.starts_with(q.terms.front())) {
        return 1;
    }
    return 2;
}

bool wants_hidden(const Query& q) {
    if (q.kind == Query::Kind::words) {
        return std::ranges::any_of(q.terms, [](const std::string& t) { return t.starts_with('.'); });
    }
    return q.text.starts_with('.');
}

bool glob_match(std::string_view pattern, std::string_view name, bool casefold) {
    constexpr auto npos = std::string_view::npos;
    auto same = [casefold](char a, char b) { return casefold ? lower(a) == lower(b) : a == b; };
    auto in_range = [casefold](char lo, char hi, char c) {
        if (lo <= c && c <= hi) return true;
        return casefold && ((lo <= lower(c) && lower(c) <= hi) || (lo <= upper(c) && upper(c) <= hi));
    };

    std::size_t p = 0;
    std::size_t n = 0;
    std::size_t star_p = npos; // position after the most recent '*'
    std::size_t star_n = 0;    // name position that '*' currently covers up to

    while (n < name.size()) {
        if (p < pattern.size()) {
            const char c = pattern[p];
            if (c == '*') {
                star_p = ++p;
                star_n = n;
                continue;
            }
            if (c == '?') {
                ++p;
                ++n;
                continue;
            }
            if (c == '[') {
                std::size_t scan = p + 1;
                const bool negate =
                    scan < pattern.size() && (pattern[scan] == '!' || pattern[scan] == '^');
                if (negate) ++scan;
                if (scan < pattern.size() && pattern[scan] == ']') ++scan; // literal ']' first
                const std::size_t close = pattern.find(']', scan);
                if (close != npos) {
                    bool matched = false;
                    for (std::size_t k = p + 1 + (negate ? 1 : 0); k < close;) {
                        if (k + 2 < close && pattern[k + 1] == '-') {
                            if (in_range(pattern[k], pattern[k + 2], name[n])) matched = true;
                            k += 3;
                        } else {
                            if (same(pattern[k], name[n])) matched = true;
                            ++k;
                        }
                    }
                    if (matched != negate) {
                        p = close + 1;
                        ++n;
                        continue;
                    }
                    // no match: fall through to backtracking
                } else if (same(c, name[n])) { // unterminated '[' is literal
                    ++p;
                    ++n;
                    continue;
                }
            } else if (same(c, name[n])) {
                ++p;
                ++n;
                continue;
            }
        }
        if (star_p != npos) {
            p = star_p;
            n = ++star_n;
            continue;
        }
        return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

std::string escape_glob(std::string_view literal) {
    std::string out;
    out.reserve(literal.size());
    for (const char c : literal) {
        if (c == '*' || c == '?' || c == '[' || c == ']' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

} // namespace grab
