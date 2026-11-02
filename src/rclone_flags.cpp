#include "rclone_flags.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <format>

namespace grab::rclone_flags {

namespace {

constexpr std::array connection_names{
    std::string_view("--bind"),         std::string_view("--contimeout"),  std::string_view("--timeout"),
    std::string_view("--low-level-retries"), std::string_view("--user-agent"), std::string_view("--ca-cert"),
    std::string_view("--client-cert"),  std::string_view("--client-key"),  std::string_view("--no-check-certificate"),
    std::string_view("--tpslimit"),     std::string_view("--tpslimit-burst"), std::string_view("--dscp"),
    std::string_view("--password-command"),
};

// The sftp settings that say where and as whom rclone logs in, and which host keys it trusts: grab
// gives each rclone run the server's own (engine::rclone_env), so neither a --sftp-* flag nor an
// inherited RCLONE_SFTP_* variable may replace them (each beats RCLONE_CONFIG_<NAME>_*, checked
// with rclone 1.75.1). With what each is in the server's settings, and where grab keeps it.
struct ServerSetting {
    std::string_view key; // rclone's option name
    std::string_view what;
    std::string_view in_grab; // "host": grab.conf's key; "": no key of its own; "trust": known_hosts
};
constexpr std::array server_settings{
    ServerSetting{.key = "host", .what = "address", .in_grab = "host"},
    ServerSetting{.key = "port", .what = "port", .in_grab = "port"},
    ServerSetting{.key = "user", .what = "user", .in_grab = "user"},
    ServerSetting{.key = "pass", .what = "password", .in_grab = ""},
    ServerSetting{.key = "ask_password", .what = "password", .in_grab = ""},
    ServerSetting{.key = "key_pem", .what = "key", .in_grab = "auth"},
    ServerSetting{.key = "key_file", .what = "key file", .in_grab = "key_file"},
    ServerSetting{.key = "key_file_pass", .what = "key file's passphrase", .in_grab = ""},
    ServerSetting{.key = "pubkey", .what = "certificate", .in_grab = "rclone.pubkey_file"},
    ServerSetting{.key = "pubkey_file", .what = "certificate", .in_grab = "rclone.pubkey_file"},
    ServerSetting{.key = "key_use_agent", .what = "login method (ssh-agent)", .in_grab = "auth"},
    ServerSetting{.key = "known_hosts_file", .what = "trusted host keys", .in_grab = "trust"},
    ServerSetting{.key = "ssh", .what = "connection (an ssh command in place of the SSH client)", .in_grab = "ssh"},
};

// What grab has instead of a flag that isn't about reaching the server, after "isn't used by
// ...": "" when nothing.
std::string_view instead(std::string_view name) {
    if (name == "--dry-run" || name == "-n") return "grab's own -n shows what would be fetched";
    if (name == "--bwlimit" || name == "--tpslimit" || name == "--tpslimit-burst") return "grab's own --limit sets the speed";
    if (name == "-v" || name == "-vv" || name == "--verbose") return "grab's own -v shows what it runs";
    if (name == "-P" || name == "--progress") return "grab shows its own progress";
    if (name == "--transfers") return "files at once: At once in grab's window";
    return {};
}

// rclone's settings grab's own client can't follow (guard.hpp's), as flags, with what it can't do.
struct RcloneOnly {
    std::string_view flag;
    std::string_view cant;
};
constexpr auto rclone_only = std::to_array<RcloneOnly>({
    {.flag = "--sftp-socks-proxy", .cant = "can't use a proxy"},
    {.flag = "--sftp-http-proxy", .cant = "can't use a proxy"},
    {.flag = "--sftp-subsystem", .cant = "can't use another SFTP subsystem or server command"},
    {.flag = "--sftp-server-command", .cant = "can't use another SFTP subsystem or server command"},
    {.flag = "--sftp-use-insecure-cipher", .cant = "offers only its own algorithms"},
    {.flag = "--sftp-ciphers", .cant = "offers only its own algorithms"},
    {.flag = "--sftp-key-exchange", .cant = "offers only its own algorithms"},
    {.flag = "--sftp-macs", .cant = "offers only its own algorithms"},
    {.flag = "--sftp-host-key-algorithms", .cant = "offers only its own algorithms"},
    {.flag = "--sftp-disable-concurrent-reads", .cant = "always reads several pieces at once"},
    {.flag = "--sftp-use-fstat", .cant = "can't use fstat in place of stat"},
});

// Why grab's own client doesn't use a flag after `--`, and what to use instead.
std::string grab_hint(std::string_view name, std::string_view server) {
    if (name == "--sftp-connections") return std::format("set max_connections in [{}] (grab config)", server);
    for (const std::string_view fixed : {"--sftp-chunk-size", "--sftp-concurrency", "--buffer-size", "--multi-thread-streams",
                                         "--multi-thread-cutoff", "--multi-thread-chunk-size",
                                         "--multi-thread-write-buffer-size"}) {
        if (name == fixed) {
            return "it reads 255 KiB at a time, 64 in flight, and splits big files over its connections, with nothing to "
                   "set";
        }
    }
    if (name == "--low-level-retries" || name == "--retries") return "it retries a failed piece by itself";
    for (const std::string_view other : {"--dscp", "--user-agent", "--ca-cert", "--client-cert", "--client-key",
                                         "--no-check-certificate", "--password-command"}) {
        if (name == other) return "it isn't used over SFTP";
    }
    if (name == "--sftp-idle-timeout") return "it closes idle connections itself";
    if (name == "--sftp-disable-hashcheck") return "grab's own --verify checks downloads";
    if (name == "--sftp-skip-links") return "it follows links";
    if (name == "--sftp-set-modtime") return "it never changes the server's files";
    if (name == "--sftp-set-env") return "it sets no environment on the server";
    if (const auto use = instead(name); !use.empty()) return std::string(use);
    return "it takes only --bind, --contimeout and --timeout there";
}

std::string_view name_of(std::string_view flag) { return flag.substr(0, flag.find('=')); }

bool equal_nocase(char a, char b) {
    return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b));
}

bool starts_with_nocase(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), s.begin(), equal_nocase);
}

// The server setting `rest` names: an option name as rclone writes it in a flag ('-' for '_',
// `dash`) or a variable (upper case, `!dash`; compared ignoring case).
const ServerSetting* server_setting(std::string_view rest, bool dash) {
    for (const auto& s : server_settings) {
        const bool same = std::ranges::equal(rest, s.key, [&](char a, char b) {
            return dash ? a == (b == '_' ? '-' : b) : equal_nocase(a, b);
        });
        if (same) return &s;
    }
    return nullptr;
}

} // namespace

bool is_connection(std::string_view flag) {
    const auto name = name_of(flag);
    return name.starts_with("--sftp-") || std::ranges::find(connection_names, name) != connection_names.end();
}

std::vector<std::string> connection(std::span<const std::string> flags) {
    std::vector<std::string> out;
    bool keep = false; // the last flag seen, for the values after it
    for (const auto& f : flags) {
        if (f.starts_with('-')) keep = is_connection(f);
        if (keep) out.push_back(f);
    }
    return out;
}

std::optional<Unusable> first_unusable(std::span<const std::string> extra) {
    for (const auto& f : extra) {
        if (!f.starts_with('-') || is_connection(f)) continue;
        return Unusable{.flag = f, .hint = std::string(instead(name_of(f)))};
    }
    return std::nullopt;
}

std::optional<std::string> first_unused_by_grab(std::span<const std::string> extra, std::string_view server) {
    for (std::size_t i = 0; i < extra.size(); ++i) {
        const std::string& f = extra[i];
        const auto name = name_of(f);
        if (name == "--bind" || name == "--contimeout" || name == "--timeout") {
            // Its value, as ssh::read_flags takes it.
            if (name.size() == f.size() && i + 1 < extra.size() && !extra[i + 1].starts_with('-')) ++i;
            continue;
        }
        if (!f.starts_with('-')) {
            return std::format("'{}' after -- isn't a flag; grab's own client takes only --bind, --contimeout and "
                               "--timeout there",
                               f);
        }
        if (const auto it = std::ranges::find(rclone_only, name, &RcloneOnly::flag); it != rclone_only.end()) {
            return std::format("{} after --: grab's own client {}; set engine = rclone in [{}] (grab config) to use it "
                               "through rclone",
                               name, it->cant, server);
        }
        return std::format("{} after -- isn't used by grab's own client; {}", name, grab_hint(name, server));
    }
    return std::nullopt;
}

std::optional<Refused> first_refused(std::span<const std::string> flags) {
    constexpr std::string_view sftp = "--sftp-";
    for (const auto& f : flags) {
        const auto name = name_of(f);
        if (!name.starts_with(sftp)) continue;
        if (const auto* s = server_setting(name.substr(sftp.size()), true)) {
            return Refused{.flag = std::string(name), .what = std::string(s->what), .in_grab = std::string(s->in_grab)};
        }
    }
    return std::nullopt;
}

std::string change_instead(const Refused& refused, std::string_view server) {
    if (refused.in_grab == "trust") {
        return std::format("pin its host keys with `grab server trust {}` or Trust in grab's Settings", server);
    }
    if (refused.in_grab == "ssh") return "an ssh command can't be given there";
    // Edit has no field for it: only a hand-written line, and only rclone logs in with one.
    if (refused.in_grab == "rclone.pubkey_file") {
        return std::format("write `rclone.pubkey_file = PATH` in [{}] (grab config); only rclone logs in with a "
                           "certificate (engine = rclone)",
                           server);
    }
    if (refused.in_grab.empty()) {
        return std::format("change it with `grab server edit {}` or Edit in grab's Settings (grab keeps it in its "
                           "password store)",
                           server);
    }
    return std::format("change grab.conf's `{}` with `grab server edit {}` or Edit in grab's Settings", refused.in_grab,
                       server);
}

bool kept_variable(std::string_view name) {
    constexpr std::string_view sftp = "RCLONE_SFTP_";
    if (!starts_with_nocase(name, "RCLONE_")) return true;
    if (starts_with_nocase(name, "RCLONE_CONFIG")) return false;
    if (starts_with_nocase(name, sftp)) return server_setting(name.substr(sftp.size()), false) == nullptr;
    return name.size() == 23 && starts_with_nocase(name, "RCLONE_PASSWORD_COMMAND");
}

} // namespace grab::rclone_flags
