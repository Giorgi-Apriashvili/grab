#pragma once

// The speed schedule: rules that cap the download speed at certain times of the week. They are
// set in grab's window and followed by the window and the `grab` command. In the window, a
// change of the speed limit made while a rule applies holds until the schedule's next change.
// Pure, apart from local_now().

#include "json.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace grab::schedule {

inline constexpr double min_mibps = 0.1;
inline constexpr double max_mibps = 10000;
inline constexpr std::size_t max_rules = 20;
inline constexpr int day = 24 * 60; // minutes
inline constexpr int week = 7 * day;

// From `from` to `to` (minutes since midnight, local time) on the chosen days, at most `mibps`.
// A `to` at or before `from` runs into the next day (the days are the days it starts on);
// equal times are a whole day.
struct Rule {
    unsigned days = 0x7F; // bit 0 = Monday ... bit 6 = Sunday
    int from = 18 * 60;
    int to = 0;
    double mibps = 5;
    bool operator==(const Rule&) const = default;
};
[[nodiscard]] bool valid(const Rule& r);

// The scheduled limit in MiB/s at a minute of the week (0 = Monday 00:00): the lowest speed of
// the rules that apply then; nullopt = none.
[[nodiscard]] std::optional<double> limit_at(const std::vector<Rule>& rules, int minute);

// The scheduled limit's next change after `minute`: how many minutes ahead (1 to week), and the
// limit from then on. nullopt when it never changes (no rules, or one limit all week).
struct Change {
    int minutes = 0;
    std::optional<double> limit;
};
[[nodiscard]] std::optional<Change> next_change(const std::vector<Rule>& rules, int minute);

// An rclone --bwlimit value that follows the rules by itself: a weekly timetable
// ("Mon-18:00,5120K Tue-00:00,off"), or one rate when the limit never changes. "" without
// rules.
[[nodiscard]] std::string rclone_bwlimit(const std::vector<Rule>& rules);

[[nodiscard]] std::uint64_t bytes_per_s(double mibps);

// "18:00" and "8:30" to minutes since midnight; "24:00" reads as 1440.
[[nodiscard]] std::optional<int> parse_time(std::string_view text);
[[nodiscard]] std::string time_text(int minutes); // "08:30"
// When a change `minutes_ahead` of `minute` happens, for "until ...": "21:30" later the same
// day, "24:00" at the coming midnight, else the day too ("Sat 06:00").
[[nodiscard]] std::string when_text(int minute, int minutes_ahead);
[[nodiscard]] std::string speed_text(double mibps); // "5 MiB/s", "2.5 MiB/s"

// [{"days":["mon","tue"],"from":"18:00","to":"00:00","mibps":5}, ...]. Reading drops invalid
// rules and those beyond max_rules.
[[nodiscard]] json::Value to_json(const std::vector<Rule>& rules);
[[nodiscard]] std::vector<Rule> from_json(const json::Value& v);

// The window's speed limit setting.
struct Limit {
    bool on = false;
    double mibps = 5;
    bool operator==(const Limit&) const = default;
};

// A change of the limit made while the schedule limited the speed. It holds while the schedule
// keeps the limit it replaced, and at most until `until` (Unix seconds: the schedule's next
// change when it was made, for a window that was closed in between).
struct Override {
    Limit limit;
    double scheduled = 0;
    std::int64_t until = 0;
    bool operator==(const Override&) const = default;
};
[[nodiscard]] bool holds(const Override& o, std::optional<double> scheduled, std::int64_t now);

// What applies: an override that holds, else the scheduled limit, else the user's own setting.
enum class Source { own, schedule, change };
struct Applied {
    Limit limit;
    Source source = Source::own;
};
[[nodiscard]] Applied apply(const Limit& own, std::optional<double> scheduled, const std::optional<Override>& change,
                            std::int64_t now);

// The local time of the week and the Unix time, now.
struct Now {
    int minute = 0; // of the week, 0 = Monday 00:00
    int second = 0; // within that minute
    std::int64_t unix = 0;
};
[[nodiscard]] Now local_now();

} // namespace grab::schedule
