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
    case server_conf::Auth::stored_key:
        return util::failf("{} logs in with a key kept in grab's password store (imported from rclone.conf), which "
                           "grab's own client can't use yet: it works on engine = rclone only",
                           name);
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

std::expected<std::vector<std::string>, std::string> apply_flags(Target& target, std::span<const std::string> flags) {
    std::vector<std::string> unused;
    for (std::size_t i = 0; i < flags.size(); ++i) {
        const std::string& flag = flags[i];
        const std::string name = flag.substr(0, flag.find('='));
        if (name != "--bind" && name != "--contimeout" && name != "--timeout") {
            unused.push_back(flag);
            continue;
        }
        std::string value;
        if (name.size() < flag.size()) value = flag.substr(name.size() + 1);
        else if (i + 1 < flags.size() && !flags[i + 1].starts_with('-')) value = flags[++i];
        else return util::failf("{} needs a value", name);
        if (name == "--bind") {
            target.bind = std::string(util::trim(value));
            continue;
        }
        const auto time = parse_duration(value);
        if (!time) return util::failf("{} {}: not a time grab can read (such as 30s, 1m30s or off)", name, value);
        (name == "--contimeout" ? target.connect_timeout : target.timeout) = *time;
    }
    return unused;
}

} // namespace grab::ssh
