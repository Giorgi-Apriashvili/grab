#include "rclone_flags.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <utility>

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
    ServerSetting{.key = "ssh", .what = "connection (an ssh command instead of rclone's own)", .in_grab = "ssh"},
};

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
        const auto name = name_of(f);
        std::string hint = "add --rclone for a plain rclone copy that uses it";
        if (name == "--dry-run" || name == "-n") hint = "grab's own -n shows what would be fetched";
        else if (name == "--bwlimit") hint = "grab's own --limit sets the speed";
        else if (name == "-v" || name == "-vv" || name == "--verbose") hint = "grab's own -v shows the rclone commands";
        else if (name == "-P" || name == "--progress") hint = "grab shows its own progress";
        return Unusable{f, std::move(hint)};
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
    if (refused.in_grab == "ssh") return "grab's runs use rclone's own SSH client, which takes no ssh command";
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
