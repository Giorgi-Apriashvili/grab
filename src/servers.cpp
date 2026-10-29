#include "servers.hpp"

#include "rclone.hpp"
#include "util.hpp"

#include <openssl/evp.h>
#include <openssl/hmac.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <format>
#include <set>

namespace grab::servers {

std::optional<std::string> validate_name(std::string_view name) {
    if (name.empty()) return "a name is required";
    if (name.size() > 64) return "use at most 64 characters";
    if (util::to_lower(name) == "grab") return "\"grab\" is reserved for grab.conf's own section";
    if (name.front() == '-' || name.front() == '.') return "the name cannot start with - or .";
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '_' || c == '-' || c == '.')) {
            return "use letters, digits, _ - and . only";
        }
    }
    return std::nullopt;
}

// ---- host keys -------------------------------------------------------------------------------

std::string key_label(std::string_view type) {
    if (type == "ssh-ed25519") return "ED25519";
    if (type.starts_with("ecdsa-")) return "ECDSA";
    if (type == "ssh-rsa") return "RSA";
    return std::string(type);
}

bool names_host(std::string_view name, std::string_view host, int port) {
    // Plain names as written: libssh2, rclone and pinned_algorithms all compare them exactly, so a
    // line for "BOX.example" is another host's pin than one for "box.example".
    const std::string bracketed = std::format("[{}]:{}", host, port);
    if (!name.starts_with("|1|")) return name == bracketed || (port == 22 && name == host);
    // "|1|salt|hash": HMAC-SHA1 of the name under the salt, both in base64 (OpenSSH's
    // HashKnownHosts). OpenSSH hashes the name lower-cased; as written is tried too.
    name.remove_prefix(3);
    const auto bar = name.find('|');
    if (bar == std::string_view::npos) return false;
    const auto salt = util::base64_decode(name.substr(0, bar));
    const auto hash = util::base64_decode(name.substr(bar + 1));
    if (!salt || !hash || hash->size() != 20) return false;
    std::vector<std::string> candidates{std::format("[{}]:{}", util::to_lower(host), port), bracketed};
    if (port == 22) candidates.insert(candidates.end(), {util::to_lower(host), std::string(host)});
    for (const auto& candidate : candidates) {
        std::array<unsigned char, EVP_MAX_MD_SIZE> mac{};
        unsigned int len = 0;
        if (HMAC(EVP_sha1(), salt->data(), static_cast<int>(salt->size()),
                 reinterpret_cast<const unsigned char*>(candidate.data()), candidate.size(), mac.data(), &len) &&
            len == hash->size() && std::memcmp(mac.data(), hash->data(), len) == 0) {
            return true;
        }
    }
    return false;
}

std::string forget_host(std::string_view text, std::string_view host, int port) {
    std::string out;
    out.reserve(text.size());
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        end = end == std::string_view::npos ? text.size() : end + 1;
        const std::string_view line = text.substr(start, end - start); // with its line break
        start = end;
        // The host list: the first field after any indent. Comments and marker lines
        // (@revoked, @cert-authority) aren't pins and stay.
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string_view::npos || line[first] == '#' || line[first] == '@' || line[first] == '\r' ||
            line[first] == '\n') {
            out += line;
            continue;
        }
        auto last = line.find_first_of(" \t\r\n", first);
        if (last == std::string_view::npos) last = line.size();
        std::vector<std::string_view> kept;
        bool dropped = false;
        for (std::size_t b = first; b <= last;) {
            const std::size_t e = std::min(line.find(',', b), last);
            const auto name = line.substr(b, e - b);
            if (names_host(name, host, port)) dropped = true;
            else if (!name.empty()) kept.push_back(name);
            b = e + 1;
        }
        if (!dropped) {
            out += line;
        } else if (!kept.empty()) {
            out += line.substr(0, first);
            for (std::size_t i = 0; i < kept.size(); ++i) {
                if (i > 0) out += ',';
                out += kept[i];
            }
            out += line.substr(last);
        }
    }
    return out;
}

std::string merge_known_hosts(std::string_view existing, const std::vector<std::string>& lines) {
    std::set<std::string, std::less<>> present;
    std::size_t start = 0;
    while (start < existing.size()) {
        auto end = existing.find('\n', start);
        if (end == std::string_view::npos) end = existing.size();
        present.emplace(util::trim(existing.substr(start, end - start)));
        start = end + 1;
    }
    std::string out(existing);
    if (!out.empty() && !out.ends_with('\n')) out += '\n';
    for (const auto& l : lines) {
        const std::string line(util::trim(l));
        if (line.empty() || present.contains(line)) continue;
        out += line;
        out += '\n';
        present.insert(line);
    }
    return out;
}

// ---- rclone command lines --------------------------------------------------------------------

std::vector<std::string> probe_argv(const std::string& rclone, const std::string& name) {
    return {rclone, "lsf", name + ":", "--dirs-only", "--max-depth", "1", "--contimeout", "15s",
            "--timeout", "30s", "--low-level-retries", "1", "--retries", "1", "--config",
            std::string(config_in_memory)};
}

// ---- grab.conf text edits --------------------------------------------------------------------

namespace {

struct Lines {
    std::vector<std::string> lines; // without line breaks
    std::string eol = "\n";
    bool final_newline = true;
};

Lines split_lines(std::string_view text) {
    Lines l;
    if (text.find("\r\n") != std::string_view::npos) l.eol = "\r\n";
    l.final_newline = text.empty() || text.ends_with('\n');
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view line = text.substr(start, end - start);
        if (line.ends_with('\r')) line.remove_suffix(1);
        l.lines.emplace_back(line);
        start = end + 1;
    }
    return l;
}

std::string join_lines(const Lines& l) {
    std::string out;
    for (std::size_t i = 0; i < l.lines.size(); ++i) {
        out += l.lines[i];
        if (i + 1 < l.lines.size() || l.final_newline) out += l.eol;
    }
    return out;
}

bool is_header(std::string_view line) {
    const auto t = util::trim(line);
    return t.size() >= 2 && t.front() == '[' && t.back() == ']';
}

bool is_header_of(std::string_view line, std::string_view name) {
    const auto t = util::trim(line);
    return is_header(t) && util::trim(t.substr(1, t.size() - 2)) == name;
}

bool blank(std::string_view line) { return util::trim(line).empty(); }

// The key a "key = value" line sets; nullopt for a comment, a blank or another line.
std::optional<std::string_view> key_of(std::string_view line) {
    const auto t = util::trim(line);
    if (t.empty() || t.starts_with('#') || t.starts_with(';')) return std::nullopt;
    const auto eq = t.find('=');
    if (eq == std::string_view::npos) return std::nullopt;
    return util::trim(t.substr(0, eq));
}

// [begin, end) of a section's first block at or after line `from`, header included; nullopt when
// there is none.
std::optional<std::pair<std::size_t, std::size_t>> find_section(const std::vector<std::string>& lines,
                                                                std::string_view name, std::size_t from = 0) {
    auto comment = [](std::string_view line) {
        const auto t = util::trim(line);
        return t.starts_with('#') || t.starts_with(';');
    };
    for (std::size_t i = from; i < lines.size(); ++i) {
        if (!is_header_of(lines[i], name)) continue;
        std::size_t end = i + 1;
        while (end < lines.size() && !is_header(lines[end])) ++end;
        if (end < lines.size()) {
            // A comment block directly above the next header, set off from this section by a
            // blank line, is that section's lead-in ("# rclone remote [next]: ..."), not ours.
            std::size_t j = end;
            while (j > i + 1 && comment(lines[j - 1])) --j;
            if (j < end && j > i + 1 && blank(lines[j - 1])) end = j;
        }
        while (end > i + 1 && blank(lines[end - 1])) --end;
        return std::pair{i, end};
    }
    return std::nullopt;
}

// Removes the block [begin, end) of `section`, with grab's lead-in comment right above it.
void remove_block(Lines& l, std::string_view section, std::pair<std::size_t, std::size_t> range) {
    auto [begin, end] = range;
    // grab's generated lead-in comment ("# rclone remote [name]: ...").
    if (begin > 0) {
        const auto prev = util::trim(l.lines[begin - 1]);
        if (prev.starts_with(std::format("# rclone remote [{}]", section)) ||
            prev.starts_with(std::format("# rclone.conf [{}]", section))) {
            --begin;
        }
    }
    l.lines.erase(l.lines.begin() + static_cast<std::ptrdiff_t>(begin),
                  l.lines.begin() + static_cast<std::ptrdiff_t>(end));
    // Don't leave two blank lines where the section was.
    if (begin > 0 && begin <= l.lines.size() && blank(l.lines[begin - 1]) &&
        (begin == l.lines.size() || blank(l.lines[begin]))) {
        l.lines.erase(l.lines.begin() + static_cast<std::ptrdiff_t>(begin - 1));
    }
    // Nor a blank first line when the removed section opened the file.
    while (begin == 0 && !l.lines.empty() && blank(l.lines.front())) l.lines.erase(l.lines.begin());
}

} // namespace

std::string remove_section(std::string_view text, std::string_view section) {
    Lines l = split_lines(text);
    if (!find_section(l.lines, section)) return std::string(text);
    // Every block of that name: ini::parse merges them into one section (review: a second [Box]
    // lower down would otherwise keep the server listed).
    while (auto range = find_section(l.lines, section)) {
        remove_block(l, section, *range);
    }
    while (!l.lines.empty() && blank(l.lines.back())) l.lines.pop_back();
    l.final_newline = true;
    return join_lines(l);
}

std::string append_section(std::string_view text, std::string_view section_text) {
    Lines l = split_lines(text);
    while (!l.lines.empty() && blank(l.lines.back())) l.lines.pop_back();
    if (!l.lines.empty()) l.lines.emplace_back();
    Lines added = split_lines(section_text);
    l.lines.insert(l.lines.end(), added.lines.begin(), added.lines.end());
    l.final_newline = true;
    return join_lines(l);
}

std::expected<std::string, std::string> set_value(std::string_view text, std::string_view section,
                                                  std::string_view key, std::string_view value,
                                                  std::span<const std::string_view> after) {
    for (const auto& [what, part] : {std::pair{"section name", section}, std::pair{"key", key}}) {
        if (auto bad = server_conf::check_value(part)) return util::failf("a grab.conf {}: {}", what, *bad);
    }
    if (auto bad = server_conf::check_value(value)) return util::failf("[{}] {}: {}", section, key, *bad);
    Lines l = split_lines(text);
    const std::string assignment = value.empty() ? std::format("{} =", key) : std::format("{} = {}", key, value);
    auto range = find_section(l.lines, section);
    if (!range) {
        std::vector<std::string> head{std::format("[{}]", section), assignment};
        if (!l.lines.empty()) head.emplace_back();
        l.lines.insert(l.lines.begin(), head.begin(), head.end());
        l.final_newline = true;
        return join_lines(l);
    }
    // As ini::parse reads it, the last line of the key across every block of that name wins: that
    // one is replaced and the earlier ones go, so the value written is the one read back.
    std::vector<std::size_t> lines_of_key;
    std::pair<std::size_t, std::size_t> last = *range;
    for (auto block = range; block; block = find_section(l.lines, section, block->second)) {
        last = *block;
        for (std::size_t i = block->first + 1; i < block->second; ++i) {
            if (key_of(l.lines[i]) == key) lines_of_key.push_back(i);
        }
    }
    if (!lines_of_key.empty()) {
        l.lines[lines_of_key.back()] = assignment;
        for (auto i = lines_of_key.size() - 1; i-- > 0;) {
            l.lines.erase(l.lines.begin() + static_cast<std::ptrdiff_t>(lines_of_key[i]));
        }
        return join_lines(l);
    }
    // A new key goes in the last block, after `after`'s last one found there.
    const auto [begin, end] = last;
    std::size_t at = begin + 1;
    for (std::size_t i = begin + 1; i < end; ++i) {
        if (const auto name = key_of(l.lines[i]); name && std::ranges::find(after, *name) != after.end()) {
            at = std::max(at, i + 1);
        }
    }
    l.lines.insert(l.lines.begin() + static_cast<std::ptrdiff_t>(at), assignment);
    return join_lines(l);
}

std::string replace_lead_in(std::string_view text, std::string_view section, std::string_view comment) {
    Lines l = split_lines(text);
    auto range = find_section(l.lines, section);
    if (!range || range->first == 0) return std::string(text);
    auto& prev = l.lines[range->first - 1];
    if (!util::trim(prev).starts_with("# rclone remote [")) return std::string(text);
    prev = comment;
    return join_lines(l);
}

} // namespace grab::servers
