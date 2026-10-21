#include "rclone_flags.hpp"

#include <algorithm>
#include <array>
#include <cctype>
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

std::string_view name_of(std::string_view flag) { return flag.substr(0, flag.find('=')); }

bool starts_with_nocase(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), s.begin(), [](char a, char b) {
               return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b));
           });
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

bool kept_variable(std::string_view name) {
    if (!starts_with_nocase(name, "RCLONE_")) return true;
    return starts_with_nocase(name, "RCLONE_CONFIG") || starts_with_nocase(name, "RCLONE_SFTP_") ||
           (name.size() == 23 && starts_with_nocase(name, "RCLONE_PASSWORD_COMMAND"));
}

} // namespace grab::rclone_flags
