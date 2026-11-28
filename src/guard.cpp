#include "guard.hpp"

#include "passwords.hpp"
#include "proxy.hpp"
#include "ssh.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <expected>
#include <format>

namespace grab::guard {

namespace {

// rclone 1.75.1's sftp options that keep grab off a server, with the kind of reason each is when
// its value says so; `flag` options are booleans (true decides).
struct Option {
    std::string_view key;
    Kind kind;
    bool flag = false;
};
constexpr auto options = std::to_array<Option>({
    {.key = "socks_proxy", .kind = Kind::route},
    {.key = "http_proxy", .kind = Kind::route},
    {.key = "ssh", .kind = Kind::route},
    {.key = "subsystem", .kind = Kind::server},
    {.key = "server_command", .kind = Kind::server},
    {.key = "pubkey", .kind = Kind::certificate},
    {.key = "pubkey_file", .kind = Kind::certificate},
    {.key = "use_insecure_cipher", .kind = Kind::cipher, .flag = true},
    {.key = "ciphers", .kind = Kind::cipher},
    {.key = "key_exchange", .kind = Kind::cipher},
    {.key = "macs", .kind = Kind::cipher},
    {.key = "host_key_algorithms", .kind = Kind::cipher},
    {.key = "disable_concurrent_reads", .kind = Kind::quirk, .flag = true},
    {.key = "use_fstat", .kind = Kind::quirk, .flag = true},
});

// The inherited RCLONE_SFTP_<KEY> variables that never reached rclone: grab left them out of its
// rclone runs (0.20's rclone_flags::kept_variable), as they would have replaced the server's own
// settings. They never routed a server, so they don't count now either.
constexpr auto unseen_variables = std::to_array<std::string_view>({"ssh", "pubkey", "pubkey_file"});

// rclone's proxies (PROXIES.md): in grab's store a server's route (route()), anywhere else a
// reason, as grab reads a proxy only from its own setting.
constexpr auto proxy_keys = std::to_array<std::string_view>({"socks_proxy", "http_proxy"});
// The key of two kept proxies' problem.
constexpr std::string_view both_proxies = "proxies";

bool proxy_reason(const Reason& r) { return std::ranges::contains(proxy_keys, r.key) || r.key == both_proxies; }

const Option* option(std::string_view key) {
    const auto it = std::ranges::find(options, key, &Option::key);
    return it == options.end() ? nullptr : &*it;
}

// Go's strconv.ParseBool, as rclone reads a boolean.
bool go_true(std::string_view s) {
    for (const std::string_view t : {"1", "t", "T", "TRUE", "true", "True"}) {
        if (s == t) return true;
    }
    return false;
}

// What grab's client offers for an algorithm list rclone can narrow.
std::vector<std::string> offered(std::string_view key) {
    if (key == "host_key_algorithms") return {ssh::scan_algorithms.begin(), ssh::scan_algorithms.end()};
    const std::string_view list = key == "ciphers" ? ssh::cipher_algorithms
                                  : key == "macs"  ? ssh::mac_algorithms
                                                   : ssh::kex_algorithms;
    return util::split(list, ',');
}

// The kind of reason `value` of `key` is, if it keeps grab's client off.
std::optional<Kind> verdict(const Option& o, std::string_view value) {
    value = util::trim(value);
    if (o.flag) return go_true(value) ? std::optional(o.kind) : std::nullopt;
    if (value.empty()) return std::nullopt;
    if (o.key == "subsystem") return value == "sftp" ? std::nullopt : std::optional(o.kind);
    if (o.kind == Kind::cipher) {
        // rclone's lists are space-separated; commas are taken too.
        const auto offer = offered(o.key);
        std::string items(value);
        std::ranges::replace(items, ',', ' ');
        for (const auto& item : util::split_args(items)) {
            if (std::ranges::contains(offer, item)) return std::nullopt;
        }
    }
    return o.kind;
}

void set(Settings& settings, const Option& o, std::string_view value, Origin origin, std::string setting,
         std::string where) {
    settings[std::string(o.key)] =
        Found{.where = std::move(where), .origin = origin, .setting = std::move(setting), .blocks = verdict(o, value)};
}

// rclone's --sftp-<key> flags in `flags`, as pflag reads them: "--flag=value", a boolean alone, or
// a string flag and the next argument. `where` makes the setting's place from the flag's name.
void read_flags(Settings& settings, std::span<const std::string> flags, Origin origin,
                const std::function<std::string(std::string_view)>& where) {
    constexpr std::string_view sftp = "--sftp-";
    for (std::size_t i = 0; i < flags.size(); ++i) {
        const std::string_view flag = flags[i];
        if (!flag.starts_with(sftp)) continue;
        const auto eq = flag.find('=');
        const std::string_view name = flag.substr(0, eq);
        std::string key(name.substr(sftp.size()));
        std::ranges::replace(key, '-', '_');
        const Option* o = option(key);
        if (o == nullptr) continue;
        std::string_view value;
        if (eq != std::string_view::npos) value = flag.substr(eq + 1);
        else if (o->flag) value = "true";
        else if (i + 1 < flags.size()) value = flags[++i];
        set(settings, *o, value, origin, std::string(name), where(name));
    }
}

// ssh.exe's -o ProxyCommand / ProxyJump and -J in `ssh_options` (OpenSSH keeps the first value).
void read_ssh_options(Settings& settings, std::span<const std::string> ssh_options) {
    auto take = [&](std::string key, std::string_view value, std::string setting) {
        if (settings.contains(key)) return;
        value = util::trim(value);
        const bool route = !value.empty() && util::to_lower(value) != "none";
        settings[std::move(key)] = Found{.where = "ssh_options' " + setting,
                                         .origin = Origin::ssh_options,
                                         .setting = setting,
                                         .blocks = route ? std::optional(Kind::route) : std::nullopt};
    };
    for (std::size_t i = 0; i < ssh_options.size(); ++i) {
        const std::string_view arg = ssh_options[i];
        if (arg.starts_with("-J")) {
            const std::string_view value = arg.size() > 2 ? arg.substr(2)
                                           : i + 1 < ssh_options.size() ? std::string_view(ssh_options[++i])
                                                                          : std::string_view();
            take("ProxyJump", value, "-J");
            continue;
        }
        if (!arg.starts_with("-o")) continue;
        const std::string_view o = arg.size() > 2 ? arg.substr(2)
                                   : i + 1 < ssh_options.size() ? std::string_view(ssh_options[++i])
                                                                  : std::string_view();
        // "Keyword=value" or "Keyword value"; keywords ignore case.
        const auto sep = o.find_first_of("= \t");
        const std::string keyword = util::to_lower(util::trim(o.substr(0, sep)));
        std::string_view value = sep == std::string_view::npos ? std::string_view() : o.substr(sep + 1);
        value = util::trim(value);
        if (value.starts_with('=')) value = util::trim(value.substr(1));
        if (keyword == "proxycommand") take("ProxyCommand", value, "-o ProxyCommand");
        if (keyword == "proxyjump") take("ProxyJump", value, "-o ProxyJump");
    }
}

std::string upper(std::string_view key) {
    std::string out(key);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// "a", "a and b", "a, b and c".
std::string listed(const std::vector<std::string>& items) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += i + 1 == items.size() ? " and " : ", ";
        out += items[i];
    }
    return out;
}

// How to clear a proxy where grab doesn't take one: its own setting instead (PROXIES.md 2, the
// other old sources), or the server's proxy set again with Edit (a kept one it won't use).
std::string clear_proxy_text(std::string_view server, const Reason& r) {
    const std::string line = std::format("`proxy = {}://HOST:PORT` in [{}]", r.key == "http_proxy" ? "http" : "socks5",
                                         server);
    switch (r.origin) {
    case Origin::store:
        if (r.key == both_proxies) return std::format("Edit {} and choose one of them, or Directly", server);
        return std::format("Edit {} and set its proxy again, or choose Directly", server);
    case Origin::line: return std::format("write {} and delete the {} line (grab config)", line, r.setting);
    case Origin::environment:
        return std::format("grab doesn't follow proxy variables: set the proxy per server ({}) and clear {} for grab "
                           "(`set {}=` in that window before starting grab) or for good",
                           line, r.setting, r.setting);
    case Origin::common_flags:
        return std::format("write {} and take {} out of common_flags (grab config)", line, r.setting);
    case Origin::folder_flags:
        return std::format("write {} and take {} out of folder_flags (grab config)", line, r.setting);
    case Origin::ssh_options:
    case Origin::unreadable: break;
    }
    return {};
}

// How to clear reason `r` for `server`; `kept`: every setting of the server the import kept in
// grab's store, which one way out clears.
std::string clear_text(std::string_view server, const Reason& r, const std::vector<std::string>& kept) {
    if (proxy_reason(r)) {
        if (auto how = clear_proxy_text(server, r); !how.empty()) return how;
    }
    switch (r.origin) {
    case Origin::store:
        return std::format("the import kept {} in grab's store: remove {} and add it again (Remove and Add in grab's "
                           "Settings, or `grab server remove {}` and `grab server add`)",
                           listed(kept), server, server);
    case Origin::line: return std::format("delete the {} line in [{}] (grab config)", r.setting, server);
    case Origin::environment:
        return std::format("{} is set in grab's environment and covers every server; clear it for grab alone (`set {}=` "
                           "in that window before starting grab) or for good",
                           r.setting, r.setting);
    case Origin::common_flags: return std::format("take {} out of common_flags in [{}] (grab config)", r.setting, server);
    case Origin::folder_flags: return std::format("take {} out of folder_flags in [{}] (grab config)", r.setting, server);
    case Origin::ssh_options: return std::format("delete {} from ssh_options in [{}] (grab config)", r.setting, server);
    case Origin::unreadable: break;
    }
    return std::format("saving {} with Edit (no change needed) sets the unreadable store aside, and with it any proxy "
                       "or ssh command kept there: {} then connects directly, or through the proxy its grab.conf "
                       "section gives it",
                       server, server);
}

} // namespace

std::vector<std::pair<std::string, std::string>> store_extras(const passwords::Store& store, std::string_view server,
                                                             const passwords::Binding& login) {
    std::vector<std::pair<std::string, std::string>> out;
    const auto it = store.find(server);
    if (it == store.end()) return out;
    for (const auto& [key, secret] : it->second) {
        if (key.starts_with(passwords::rclone_prefix) && secret.binding == login) {
            out.emplace_back(key.substr(passwords::rclone_prefix.size()), secret.value);
        }
    }
    return out;
}

Sources sources(const RemoteSettings& settings, const server_conf::Connection& connection,
                std::vector<std::pair<std::string, std::string>> extras) {
    return Sources{.extras = std::move(extras),
                   .lines = connection.rclone,
                   .environment = [](const std::string& name) { return util::getenv_utf8(name.c_str()); },
                   .common_flags = settings.common_flags,
                   .folder_flags = settings.folder_flags,
                   .ssh_options = settings.ssh_options};
}

Settings evaluate(const Sources& sources) {
    Settings settings;
    for (const auto& [key, value] : sources.extras) {
        if (std::ranges::contains(proxy_keys, key)) continue; // the server's route (route()), never a reason
        if (const Option* o = option(key)) {
            set(settings, *o, value, Origin::store, "rclone." + key, std::format("rclone.{} in grab's store", key));
        }
    }
    for (const auto& [key, value] : sources.lines) {
        if (const Option* o = option(key)) set(settings, *o, value, Origin::line, "rclone." + key, "rclone." + key);
    }
    if (sources.environment) {
        for (const auto& o : options) {
            if (std::ranges::contains(unseen_variables, o.key)) continue;
            const std::string name = "RCLONE_SFTP_" + upper(o.key);
            const auto value = sources.environment(name);
            if (value && !value->empty()) set(settings, o, *value, Origin::environment, name, name);
        }
    }
    read_flags(settings, sources.common_flags, Origin::common_flags,
               [](std::string_view flag) { return std::format("common_flags' {}", flag); });
    read_flags(settings, sources.folder_flags, Origin::folder_flags,
               [](std::string_view flag) { return std::format("folder_flags' {}", flag); });
    read_ssh_options(settings, sources.ssh_options);
    return settings;
}

void add_unreadable_store(Settings& settings) {
    settings["store"] = Found{.where = "grab's store, which can't be read",
                              .origin = Origin::unreadable,
                              .setting = {},
                              .blocks = Kind::route};
}

std::vector<Reason> reasons(const Settings& settings) {
    std::vector<Reason> out;
    for (const auto& [key, found] : settings) {
        // ssh_options' routes only ever took ssh.exe's searches: grab connects directly, as the
        // downloads always did, and a one-time note says so (decision 4).
        if (!found.blocks || found.origin == Origin::ssh_options) continue;
        out.push_back(Reason{.kind = *found.blocks, .key = key, .where = found.where, .origin = found.origin,
                             .setting = found.setting});
    }
    return out;
}

passwords::Binding proxy_login(const proxy::Proxy& proxy) {
    return passwords::Binding{.user = proxy.user, .host = proxy.host, .port = proxy.port};
}

std::expected<Route, RouteProblem> route(const passwords::Store& store, std::string_view server,
                                         const server_conf::Connection& c, bool any_login) {
    if (c.proxy.kind != proxy::Kind::none) {
        Route out{.proxy = c.proxy, .from = ProxyFrom::line, .key = {}};
        out.proxy.password.clear();
        // Sent only with a user (proxy.hpp); bound to this very proxy, so a hand edit of its
        // address or user finds none.
        if (!out.proxy.user.empty()) {
            out.proxy.password =
                passwords::value_for(store, server, passwords::proxy_password, proxy_login(out.proxy)).value_or("");
        }
        return out;
    }
    std::vector<std::pair<std::string_view, const passwords::Secret*>> kept;
    for (const std::string_view key : proxy_keys) {
        const auto* secret = passwords::find(store, server, std::string(passwords::rclone_prefix) + std::string(key));
        // A blank one is no proxy, as rclone read it.
        if (secret != nullptr && !util::trim(secret->value).empty()) kept.emplace_back(key, secret);
    }
    if (kept.empty()) return Route{};
    if (kept.size() > 1) {
        const std::string both = "rclone.socks_proxy and rclone.http_proxy";
        return std::unexpected(RouteProblem{.key = std::string(both_proxies),
                                            .found = Found{.where = both + " in grab's store",
                                                           .origin = Origin::store,
                                                           .setting = both,
                                                           .blocks = Kind::route}});
    }
    const auto [key, secret] = kept.front();
    const std::string setting = std::string(passwords::rclone_prefix) + std::string(key);
    // Never the value in the reason: it can hold a user:password.
    auto refused = [&](std::string_view why) {
        return std::unexpected(RouteProblem{.key = std::string(key),
                                            .found = Found{.where = std::format("{} in grab's store, {}", setting, why),
                                                           .origin = Origin::store,
                                                           .setting = setting,
                                                           .blocks = Kind::route}});
    };
    const passwords::Binding login{.user = c.user, .host = c.host, .port = c.port};
    if (!any_login && secret->binding != login) return refused("saved for another login");
    const auto value = util::trim(secret->value);
    auto parsed = key == "socks_proxy" ? proxy::parse_rclone_socks(value) : proxy::parse_rclone_http(value);
    if (!parsed) return refused("which grab can't use: " + parsed.error());
    return Route{.proxy = std::move(*parsed), .from = ProxyFrom::kept, .key = std::string(key)};
}

void add_route_problem(Settings& settings, const RouteProblem& problem) {
    // One that refuses already says enough (common_flags' --sftp-socks-proxy).
    auto& found = settings[problem.key];
    if (!found.blocks) found = problem.found;
}

std::string route_text(std::string_view server, const RouteProblem& problem) {
    Settings settings;
    add_route_problem(settings, problem);
    return text(server, reasons(settings));
}

std::string describe(const Reason& reason) {
    const std::string_view k = reason.key;
    if (proxy_reason(reason)) {
        const std::string_view kind = k == "socks_proxy"  ? "a SOCKS proxy"
                                      : k == "http_proxy" ? "an HTTP proxy"
                                                          : "both a SOCKS and an HTTP proxy";
        switch (reason.origin) {
        case Origin::store: return std::format("has {} ({})", kind, reason.where);
        case Origin::environment: return std::format("has {} in a variable grab doesn't follow ({})", kind, reason.where);
        default: return std::format("has {} where grab doesn't read one ({})", kind, reason.where);
        }
    }
    const char* what = k == "ssh"                    ? "connects through an ssh command"
                       : k == "ProxyCommand"         ? "connects through a ProxyCommand"
                       : k == "ProxyJump"            ? "connects through a jump host"
                       : k == "store"                ? "may connect through a proxy or an ssh command"
                       : k == "subsystem"            ? "uses another SFTP subsystem"
                       : k == "server_command"       ? "starts its SFTP server with a command of its own"
                       : k == "pubkey" || k == "pubkey_file" ? "logs in with a certificate"
                       : k == "use_insecure_cipher"  ? "needs insecure ciphers"
                       : k == "ciphers"              ? "needs other ciphers"
                       : k == "key_exchange"         ? "needs other key exchanges"
                       : k == "macs"                 ? "needs other MACs"
                       : k == "host_key_algorithms"  ? "needs other host key types"
                       : k == "disable_concurrent_reads" ? "needs one read at a time"
                       : k == "use_fstat"            ? "needs fstat in place of stat"
                                                     : "has a setting";
    return std::format("{} ({})", what, reason.where);
}

std::string text(std::string_view server, std::span<const Reason> reasons) {
    std::vector<std::string> said;
    std::vector<std::string> kept;
    for (const auto& r : reasons) {
        said.push_back(describe(r));
        if (r.origin == Origin::store && !proxy_reason(r)) kept.push_back(r.setting);
    }
    const bool route = std::ranges::any_of(reasons, [](const Reason& r) { return r.kind == Kind::route; });
    // grab can do proxies, only not from there; what an unreadable store keeps is unknown.
    const bool cant = std::ranges::any_of(reasons, [](const Reason& r) { return !proxy_reason(r) && r.key != "store"; });
    std::string out = std::format("[{}] {}{} doesn't connect to {}{}", server, listed(said),
                                  cant ? ", which grab can't, so it" : ", so grab", server,
                                  route ? " rather than go around it" : "");
    // How to clear each, each way out said once.
    std::vector<std::string> clear;
    for (const auto& r : reasons) {
        if (auto how = clear_text(server, r, kept); !std::ranges::contains(clear, how)) clear.push_back(std::move(how));
    }
    if (!clear.empty()) out += ": " + util::join(clear, "; ");
    return out;
}

} // namespace grab::guard
