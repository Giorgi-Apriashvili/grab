#include "rclone_conf.hpp"

#include "util.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <utility>

namespace grab::rclone_conf {

namespace fs = std::filesystem;

namespace {

// What %(key)s may make of a value, and how deep DEFAULT's keys may refer to each other, before
// grab gives up where rclone would run out of memory or stack.
constexpr std::size_t max_expanded = std::size_t{64} * 1024;
constexpr int max_depth = 32;
constexpr int max_lookups = 20000;
// goconfig's _DEPTH_VALUES: passes of %(key)s over one value.
constexpr int substitution_passes = 200;

// Go's strings.TrimSpace: ASCII white space and Unicode's other White_Space characters.
constexpr std::array<std::string_view, 19> wide_spaces = {
    "\xC2\x85",     "\xC2\xA0",     "\xE1\x9A\x80", "\xE2\x80\x80", "\xE2\x80\x81", "\xE2\x80\x82",
    "\xE2\x80\x83", "\xE2\x80\x84", "\xE2\x80\x85", "\xE2\x80\x86", "\xE2\x80\x87", "\xE2\x80\x88",
    "\xE2\x80\x89", "\xE2\x80\x8A", "\xE2\x80\xA8", "\xE2\x80\xA9", "\xE2\x80\xAF", "\xE2\x81\x9F",
    "\xE3\x80\x80"};

std::string_view go_trim(std::string_view s) {
    for (bool more = true; more && !s.empty();) {
        more = false;
        if (std::string_view("\t\n\v\f\r ").contains(s.front())) {
            s.remove_prefix(1);
            more = true;
            continue;
        }
        for (const auto space : wide_spaces) {
            if (s.starts_with(space)) {
                s.remove_prefix(space.size());
                more = true;
                break;
            }
        }
    }
    for (bool more = true; more && !s.empty();) {
        more = false;
        if (std::string_view("\t\n\v\f\r ").contains(s.back())) {
            s.remove_suffix(1);
            more = true;
            continue;
        }
        for (const auto space : wide_spaces) {
            if (s.ends_with(space)) {
                s.remove_suffix(space.size());
                more = true;
                break;
            }
        }
    }
    return s;
}

// goconfig's SetValue: a blank section is DEFAULT, an empty key is dropped.
void set(Document& doc, std::string_view section, std::string key, std::string value) {
    if (section.empty()) section = default_section;
    if (key.empty()) return;
    Section* s = nullptr;
    for (auto& existing : doc.sections) {
        if (existing.name == section) s = &existing;
    }
    if (s == nullptr) s = &doc.sections.emplace_back(Section{.name = std::string(section), .keys = {}, .values = {}});
    // " " is what goconfig keeps in every section it saw a header of; it lists no such key.
    if (!s->values.contains(key) && key != " ") s->keys.push_back(key);
    s->values[std::move(key)] = std::move(value);
}

// The first %(name)s in `value` (Go's regexp `%\(([^\)]+)\)s`, leftmost), or empty.
std::string_view variable(std::string_view value) {
    for (std::size_t from = 0;;) {
        const auto open = value.find("%(", from);
        if (open == std::string_view::npos) return {};
        const auto close = value.find(')', open + 2);
        if (close == std::string_view::npos) return {};
        if (close > open + 2 && close + 1 < value.size() && value[close + 1] == 's') {
            return value.substr(open, close + 2 - open);
        }
        from = open + 1;
    }
}

// The non-overlapping occurrences of `from` in `s`, as replace_all finds them.
std::size_t occurrences(std::string_view s, std::string_view from) {
    std::size_t n = 0;
    for (auto hit = s.find(from); hit != std::string_view::npos; hit = s.find(from, hit + from.size())) ++n;
    return n;
}

void replace_all(std::string& s, std::string_view from, std::string_view to) {
    std::string out;
    std::size_t at = 0;
    for (auto hit = s.find(from); hit != std::string::npos; hit = s.find(from, at)) {
        out.append(s, at, hit - at);
        out += to;
        at = hit + from.size();
    }
    out.append(s, at);
    s = std::move(out);
}

struct Budget {
    int lookups = 0;
};

// goconfig's GetValue: nullopt for no such section or key; a blank value is found.
std::expected<std::optional<Value>, std::string> lookup(const Document& doc, std::string_view section,
                                                        std::string_view key, bool parents, Budget& budget, int depth) {
    if (section.empty()) section = default_section;
    if (++budget.lookups > max_lookups || depth > max_depth) {
        return util::failf("[{}] {}: its %(key)s references go round in circles", section, key);
    }
    const Section* s = doc.find(section);
    if (s == nullptr) return std::nullopt;
    const auto it = s->values.find(key);
    if (it == s->values.end()) {
        const auto dot = section.rfind('.');
        if (parents && dot != std::string_view::npos) return lookup(doc, section.substr(0, dot), key, true, budget, depth);
        return std::nullopt;
    }
    std::string value = it->second;
    for (int pass = 0; pass < substitution_passes; ++pass) {
        const std::string found(variable(value));
        if (found.empty()) break;
        // The name as goconfig cuts it: every leading '%' and '(', every trailing ')' and 's'.
        std::string_view name = found;
        while (!name.empty() && (name.front() == '%' || name.front() == '(')) name.remove_prefix(1);
        while (!name.empty() && (name.back() == ')' || name.back() == 's')) name.remove_suffix(1);
        auto from_default = lookup(doc, default_section, name, true, budget, depth + 1);
        if (!from_default) return std::unexpected(std::move(from_default.error()));
        std::string replacement;
        if (*from_default) {
            replacement = std::move((*from_default)->text);
        } else if (section != default_section) {
            if (const auto own = s->values.find(name); own != s->values.end()) replacement = own->second;
        }
        // Checked before the string is built: one pass could otherwise take gigabytes.
        const auto n = occurrences(value, found);
        if (value.size() - (n * found.size()) + (n * replacement.size()) > max_expanded) {
            return util::failf("[{}] {}: its %(key)s references make it longer than 64 KiB", section, key);
        }
        replace_all(value, found, replacement);
    }
    return Value{.text = std::move(value), .section = std::string(section)};
}

// Go's os.Expand's variable name after a '$' and the characters it takes.
std::pair<std::string_view, std::size_t> shell_name(std::string_view s) {
    const auto special = [](char c) { return std::string_view("*#$@!?-0123456789").contains(c); };
    if (s.front() == '{') {
        if (s.size() > 2 && special(s[1]) && s[2] == '}') return {s.substr(1, 1), 3};
        for (std::size_t i = 1; i < s.size(); ++i) {
            if (s[i] == '}') {
                if (i == 1) return {{}, 2}; // "${}": eaten
                return {s.substr(1, i - 1), i + 1};
            }
        }
        return {{}, 1}; // "${" without '}': eaten
    }
    if (special(s.front())) return {s.substr(0, 1), 1};
    std::size_t i = 0;
    const auto alnum = [](char c) {
        return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    while (i < s.size() && alnum(s[i])) ++i;
    return {s.substr(0, i), i};
}

// Whether `name` is RCLONE_CONFIG_DIR as the system finds variables: ignoring case on Windows.
bool config_dir_name(std::string_view name) {
    constexpr std::string_view dir = "RCLONE_CONFIG_DIR";
#ifdef _WIN32
    return util::to_lower(name) == util::to_lower(dir);
#else
    return name == dir;
#endif
}

} // namespace

const Section* Document::find(std::string_view name) const {
    for (const auto& s : sections) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

std::expected<Document, std::string> parse(std::string_view text) {
    Document doc;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    std::string section(default_section);
    int count = 1; // goconfig names a key "-" "#1", "#2"... in each section
    int lineno = 0;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto nl = text.find('\n', start);
        const auto end = nl == std::string_view::npos ? text.size() : nl;
        const auto line = go_trim(text.substr(start, end - start));
        start = end + 1;
        ++lineno;
        if (line.empty() || line.front() == '#' || line.front() == ';') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = std::string(go_trim(line.substr(1, line.size() - 2)));
            set(doc, section, " ", " ");
            count = 1;
            continue;
        }
        if (section.empty()) return util::failf("line {}: a key in a section without a name", lineno);

        std::string_view quote;
        if (line.front() == '"') quote = line.size() >= 6 && line.starts_with(R"(""")") ? R"(""")" : R"(")";
        else if (line.front() == '`') quote = "`";
        std::string key;
        std::size_t separator = 0;
        if (!quote.empty()) {
            auto close = line.substr(quote.size()).find(quote);
            if (close == std::string_view::npos) return util::failf("line {}: a key's quote isn't closed", lineno);
            close += quote.size();
            const auto at = line.substr(close).find_first_of("=:");
            if (at == std::string_view::npos) return util::failf("line {}: no '=' or ':' after the key", lineno);
            separator = close + at;
            key = std::string(line.substr(quote.size(), close - quote.size()));
        } else {
            separator = line.find_first_of("=:");
            if (separator == std::string_view::npos || separator == 0) {
                return util::failf("line {}: expected 'key = value'", lineno);
            }
            key = std::string(go_trim(line.substr(0, separator)));
        }
        if (key == "-") key = std::format("#{}", count++);

        const auto right = go_trim(line.substr(separator + 1));
        std::string_view value_quote;
        if (right.size() >= 2 && right.front() == '`') value_quote = "`";
        else if (right.size() >= 6 && right.starts_with(R"(""")")) value_quote = R"(""")";
        std::string value;
        if (!value_quote.empty()) {
            const auto close = right.substr(value_quote.size()).rfind(value_quote);
            if (close == std::string_view::npos) return util::failf("line {}: a value's quote isn't closed", lineno);
            value = std::string(right.substr(value_quote.size(), close));
        } else {
            value = std::string(right);
        }
        set(doc, section, std::move(key), std::move(value));
    }
    return doc;
}

std::expected<std::optional<Value>, std::string> get(const Document& doc, std::string_view section,
                                                     std::string_view key, bool parents) {
    Budget budget;
    auto found = lookup(doc, section, key, parents, budget, 0);
    if (!found) return std::unexpected(std::move(found.error()));
    if (!*found || (*found)->text.empty()) return std::nullopt;
    return found;
}

std::string expand_path(std::string_view value, const fs::path& config_dir, const Getenv& getenv) {
    std::string s(value);
    const auto env_var = [&](std::string_view name) { return getenv ? getenv(name) : std::nullopt; };
    // go-homedir's Expand: "~" alone or before a separator; "~user" stays as it is.
    if (s.starts_with('~') && (s.size() == 1 || s[1] == '/' || s[1] == '\\')) {
        std::optional<std::string> home;
        const auto var = [&](std::string_view name) {
            auto v = env_var(name);
            return v && !v->empty() ? v : std::nullopt;
        };
        if (auto h = var("HOME")) home = std::move(h);
        else if (auto p = var("USERPROFILE")) home = std::move(p);
        else if (auto drive = var("HOMEDRIVE"), folder = var("HOMEPATH"); drive && folder) home = *drive + *folder;
        if (home) {
            // filepath.Join, which cleans the result.
            auto rest = std::string_view(s).substr(1);
            while (rest.starts_with('/') || rest.starts_with('\\')) rest.remove_prefix(1);
            auto joined = util::path_from_utf8(*home);
            if (!rest.empty()) joined /= util::path_from_utf8(rest);
            s = util::path_to_utf8(joined.lexically_normal());
        }
    }
    // os.ExpandEnv: what a variable gives isn't looked at again.
    const auto dir = util::path_to_utf8(config_dir);
    std::string out;
    std::size_t i = 0;
    for (std::size_t j = 0; j < s.size(); ++j) {
        if (s[j] != '$' || j + 1 == s.size()) continue;
        out.append(s, i, j - i);
        const auto [name, width] = shell_name(std::string_view(s).substr(j + 1));
        if (name.empty() && width == 0) out += '$'; // "$/" and the like: kept
        else if (!name.empty()) out += config_dir_name(name) ? dir : env_var(name).value_or("");
        j += width;
        i = j + 1;
    }
    out.append(s, std::min(i, s.size()));
    return out;
}

} // namespace grab::rclone_conf
