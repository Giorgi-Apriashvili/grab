#pragma once

// An upload whose name is taken on the server (UPLOADS.md 5.5): what is there, compared with the
// file being uploaded, and what the user's choice makes of it. The window asks first (decision 3:
// always ask, comprehensive options) unless a choice was saved as the setting.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace grab::upload {

// What to do with a file already on the server: ask the user; replace it always, when it differs
// from the one being uploaded, or when the one being uploaded is newer; keep it (skip); or keep
// both, the upload under a numbered name ("name (2).ext").
enum class Existing { ask, replace, different, newer, skip, keep_both };
[[nodiscard]] std::string_view name(Existing e); // "ask", "replace", "different", "newer", "skip", "both"
[[nodiscard]] std::optional<Existing> parse_existing(std::string_view text); // those names

// Two modification times this far apart are the same moment: FAT keeps times to 2 s, and SFTP
// gives the server's in whole seconds.
inline constexpr std::int64_t same_time_slack = 2; // seconds

// A name taken on the server: the file being uploaded and what is there.
struct Clash {
    std::string file;                        // its name
    std::uint64_t local_size = 0;
    std::int64_t local_mtime = 0;            // seconds since 1970
    std::uint64_t remote_size = 0;
    std::optional<std::int64_t> remote_mtime; // seconds since 1970; the server may not say
    bool remote_is_dir = false;
};

// How the two compare: the same (equal size, times within same_time_slack; without the server's
// time, the size decides), the one being uploaded newer or older by more than that, or neither of
// those with another size.
enum class Kind { same, newer_here, older_here, different_size };
[[nodiscard]] Kind kind(const Clash& clash);

// What happens to the upload: it replaces the file there, is skipped, goes under a numbered name
// (existing::numbered, the first one free on the server), or waits for the user's answer.
enum class Action { replace, skip, numbered, ask };
struct Decision {
    Action action = Action::ask;
    std::string note; // why a choice became something else ("a folder of that name is there")
};
// "different" replaces all but the same, "newer" only one newer here (a server that gives no time
// isn't older); a folder of that name is never replaced: Replace, if different and if newer become
// Skip with the reason.
[[nodiscard]] Decision decide(Existing choice, const Clash& clash);

} // namespace grab::upload
