#include "ssh_target.hpp"

#include "util.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>

namespace grab::ssh {

std::expected<Target, std::string> target_for(std::string_view name, const server_conf::Connection& c,
                                              const passwords::Store& store, const std::filesystem::path& known_hosts) {
    if (c.host.empty() || c.user.empty()) return util::failf("{}: its host and user are needed", name);
    Target t;
    t.host = c.host;
    t.port = c.port;
    t.user = c.user;
    t.known_hosts = known_hosts;
    const passwords::Binding login{.user = c.user, .host = c.host, .port = c.port};
    const auto saved = [&](std::string_view key) { return passwords::value_for(store, name, key, login).value_or(""); };

    switch (c.auth) {
    case server_conf::Auth::stored_key: {
        auto pem = passwords::value_for(store, name, passwords::key_pem, login);
        if (!pem) return util::failf("{}: {}", name, passwords::why_none(store, name, passwords::key_pem, login));
        auto key = unquote_key_pem(*pem);
        std::ranges::fill(*pem, '\0');
        if (!key) {
            return util::failf("{}: the key kept in grab's store isn't written as rclone reads key_pem (on one line, its "
                               "line breaks as \\n): choose a key file or ssh-agent with `grab server edit {}`",
                               name, name);
        }
        t.key_pem = std::move(*key);
        t.key_passphrase = saved(passwords::passphrase);
        break;
    }
    case server_conf::Auth::agent:
        t.agent = true;
        break;
    case server_conf::Auth::key:
        if (!c.key_file) return util::failf("{}: auth = key needs a key_file", name);
        t.key_file = util::path_from_utf8(*c.key_file);
        t.key_passphrase = saved(passwords::passphrase);
        break;
    case server_conf::Auth::password:
        if (!passwords::value_for(store, name, passwords::password, login)) {
            return util::failf("{}: {}", name, passwords::why_none(store, name, passwords::password, login));
        }
        break;
    }
    // A password saved beside a key is tried after it, as rclone does.
    t.password = saved(passwords::password);
    return t;
}

std::optional<std::string> unquote_key_pem(std::string_view value) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    // A code point as UTF-8 (valid ones only, as Go's utf8.ValidRune).
    auto append_utf8 = [](std::string& out, std::uint32_t r) {
        if (r < 0x80) {
            out += static_cast<char>(r);
        } else if (r < 0x800) {
            out += static_cast<char>(0xC0 | (r >> 6));
            out += static_cast<char>(0x80 | (r & 0x3F));
        } else if (r < 0x10000) {
            out += static_cast<char>(0xE0 | (r >> 12));
            out += static_cast<char>(0x80 | ((r >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (r & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (r >> 18));
            out += static_cast<char>(0x80 | ((r >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((r >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (r & 0x3F));
        }
    };
    std::string out;
    out.reserve(value.size());
    for (std::size_t i = 0; i < value.size();) {
        const char c = value[i];
        // A bare quote would end Go's string early, a bare line break isn't allowed in one.
        if (c == '"' || c == '\n') return std::nullopt;
        if (c != '\\') {
            out += c; // bytes over 0x7F as they are: a key is ASCII
            ++i;
            continue;
        }
        if (i + 1 >= value.size()) return std::nullopt;
        const char e = value[i + 1];
        i += 2;
        switch (e) {
        case 'a': out += '\a'; continue;
        case 'b': out += '\b'; continue;
        case 'f': out += '\f'; continue;
        case 'n': out += '\n'; continue;
        case 'r': out += '\r'; continue;
        case 't': out += '\t'; continue;
        case 'v': out += '\v'; continue;
        case '\\': out += '\\'; continue;
        case '"': out += '"'; continue;
        default: break;
        }
        if (e == 'x' || e == 'u' || e == 'U') {
            const std::size_t digits = e == 'x' ? 2 : e == 'u' ? 4 : 8;
            if (i + digits > value.size()) return std::nullopt;
            std::uint32_t v = 0;
            for (std::size_t k = 0; k < digits; ++k) {
                const int d = hex(value[i + k]);
                if (d < 0) return std::nullopt;
                v = (v << 4) | static_cast<std::uint32_t>(d);
            }
            i += digits;
            if (e == 'x') {
                out += static_cast<char>(v); // a byte
            } else {
                if (v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return std::nullopt;
                append_utf8(out, v);
            }
            continue;
        }
        if (e >= '0' && e <= '7') {
            // Three octal digits in all, at most 0377.
            if (i + 2 > value.size()) return std::nullopt;
            auto v = static_cast<std::uint32_t>(e - '0');
            for (std::size_t k = 0; k < 2; ++k) {
                const char o = value[i + k];
                if (o < '0' || o > '7') return std::nullopt;
                v = (v << 3) | static_cast<std::uint32_t>(o - '0');
            }
            if (v > 0xFF) return std::nullopt;
            i += 2;
            out += static_cast<char>(v);
            continue;
        }
        return std::nullopt; // \' and anything else Go doesn't know in a "..." string
    }
    return out;
}

std::optional<std::chrono::milliseconds> parse_duration(std::string_view text) {
    text = util::trim(text);
    if (text.empty()) return std::nullopt;
    if (text == "off") return std::chrono::milliseconds{0};
    struct Unit {
        std::string_view name;
        double ms;
    };
    static constexpr std::array<Unit, 12> units{{{.name = "ns", .ms = 1e-6},
                                                 {.name = "us", .ms = 1e-3},
                                                 {.name = "\xC2\xB5s", .ms = 1e-3}, // µs
                                                 {.name = "\xCE\xBCs", .ms = 1e-3}, // μs
                                                 {.name = "ms", .ms = 1},
                                                 {.name = "s", .ms = 1e3},
                                                 {.name = "m", .ms = 6e4},
                                                 {.name = "h", .ms = 3.6e6},
                                                 {.name = "d", .ms = 8.64e7},
                                                 {.name = "w", .ms = 6.048e8},
                                                 {.name = "M", .ms = 2.592e9},
                                                 {.name = "y", .ms = 3.1536e10}}};
    auto number_char = [](char c) { return (c >= '0' && c <= '9') || c == '.'; };
    const bool alone = std::ranges::all_of(text, number_char); // a bare number: seconds
    double total = 0;
    while (!text.empty()) {
        std::size_t n = 0;
        while (n < text.size() && number_char(text[n])) ++n;
        const std::string_view digits = text.substr(0, n);
        double value = 0;
        const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value);
        if (digits.empty() || ec != std::errc{} || ptr != digits.data() + digits.size()) return std::nullopt;
        text.remove_prefix(n);
        std::size_t u = 0;
        while (u < text.size() && !number_char(text[u])) ++u;
        const auto unit = text.substr(0, u);
        text.remove_prefix(u);
        if (unit.empty()) {
            if (!alone) return std::nullopt;
            total += value * 1e3;
            continue;
        }
        const auto found = std::ranges::find(units, unit, &Unit::name);
        if (found == units.end()) return std::nullopt;
        total += value * found->ms;
    }
    if (total > 1e13) return std::nullopt; // centuries: a typo
    // Past what Winsock's select and libssh2 take (32-bit, about 24.8 days): no limit, which is
    // what such a time means, rather than one that wraps round to a short or invalid one.
    if (total > static_cast<double>(std::numeric_limits<std::int32_t>::max())) return std::chrono::milliseconds{0};
    // Under a millisecond but not none: the least there is.
    const auto ms = static_cast<std::int64_t>(std::llround(total));
    return std::chrono::milliseconds{total > 0 && ms == 0 ? 1 : ms};
}

std::expected<ConnectionFlags, std::string> read_flags(std::span<const std::string> flags) {
    ConnectionFlags out;
    for (std::size_t i = 0; i < flags.size(); ++i) {
        const std::string& flag = flags[i];
        const std::string name = flag.substr(0, flag.find('='));
        if (name != "--bind" && name != "--contimeout" && name != "--timeout") {
            out.unused.push_back(flag);
            continue;
        }
        std::string value;
        if (name.size() < flag.size()) value = flag.substr(name.size() + 1);
        else if (i + 1 < flags.size() && !flags[i + 1].starts_with('-')) value = flags[++i];
        else return util::failf("{} needs a value", name);
        if (name == "--bind") {
            out.bind = std::string(util::trim(value));
            continue;
        }
        const auto time = parse_duration(value);
        if (!time) return util::failf("{} {}: not a time grab can read (such as 30s, 1m30s or off)", name, value);
        if (name == "--contimeout") {
            out.connect_timeout = *time;
        } else {
            out.timeout = *time;
            out.timeout_text = std::string(util::trim(value));
        }
    }
    return out;
}

std::expected<std::vector<std::string>, std::string> apply_flags(Target& target, std::span<const std::string> flags) {
    auto read = read_flags(flags);
    if (!read) return util::fail(read.error());
    if (read->bind) target.bind = *read->bind;
    if (read->connect_timeout) target.connect_timeout = *read->connect_timeout;
    if (read->timeout) target.timeout = *read->timeout;
    return std::move(read->unused);
}

} // namespace grab::ssh
