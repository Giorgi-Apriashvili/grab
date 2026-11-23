#include "json.hpp"
#include "schedule.hpp"

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

using namespace grab;
using schedule::Dir;
using schedule::Rule;

namespace {

constexpr unsigned mon = 1, tue = 2, fri = 16, sat = 32, sun = 64;
constexpr unsigned weekdays = 0x1F, every_day = 0x7F;

// A minute of the week: day 0 = Monday.
int at(int weekday, int h, int m = 0) { return weekday * schedule::day + h * 60 + m; }
constexpr int Mon = 0, Tue = 1, Thu = 3, Fri = 4, Sat = 5, Sun = 6;

// The download limit's (↓), as grab 0.23 had the only one.
std::optional<double> down_at(const std::vector<Rule>& rules, int minute) {
    return schedule::limit_at(rules, minute, Dir::down);
}
std::optional<schedule::Change> next_down(const std::vector<Rule>& rules, int minute) {
    return schedule::next_change(rules, minute, Dir::down);
}

// A rule limiting downloads only, as grab 0.23 wrote every rule.
Rule down_rule(unsigned days, int from, int to, double mibps) {
    return Rule{.days = days, .from = from, .to = to, .down = mibps, .up = std::nullopt};
}

// grab 0.23's reading of the rules (src/schedule.cpp at v0.23.0, the parts from_json needs, as
// they were): what an older grab makes of the rules this one saves in gui.json.
namespace v023 {

using json::Value;
constexpr std::array<const char*, 7> day_keys{"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
inline constexpr double min_mibps = 0.1;
inline constexpr double max_mibps = 10000;
inline constexpr std::size_t max_rules = 20;
inline constexpr int day = 24 * 60;

struct Rule {
    unsigned days = 0x7F;
    int from = 18 * 60;
    int to = 0;
    double mibps = 5;
    bool operator==(const Rule&) const = default;
};

bool valid(const Rule& r) {
    return r.days != 0 && (r.days & ~0x7Fu) == 0 && r.from >= 0 && r.from < day && r.to >= 0 && r.to < day &&
           r.mibps >= min_mibps && r.mibps <= max_mibps; // also false for NaN
}

std::optional<int> parse_time(std::string_view text) {
    const auto colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon > 2 || text.size() != colon + 3) return std::nullopt;
    int h = 0;
    int m = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == colon) continue;
        if (text[i] < '0' || text[i] > '9') return std::nullopt;
        int& part = i < colon ? h : m;
        part = part * 10 + (text[i] - '0');
    }
    if (m > 59 || h > 24 || (h == 24 && m != 0)) return std::nullopt;
    return h * 60 + m;
}

std::vector<Rule> from_json(const Value& v) {
    std::vector<Rule> rules;
    if (v.kind != Value::Kind::array) return rules;
    for (const auto& item : v.items) {
        if (rules.size() == max_rules) break;
        const Value* days = item.find("days");
        const Value* mibps = item.find("mibps");
        const auto from = parse_time(item.string_of("from").value_or(""));
        const auto to = parse_time(item.string_of("to").value_or(""));
        if (days == nullptr || days->kind != Value::Kind::array || mibps == nullptr ||
            mibps->kind != Value::Kind::number || !from || !to) {
            continue;
        }
        Rule r;
        r.days = 0;
        for (const auto& d : days->items) {
            for (std::size_t i = 0; i < day_keys.size(); ++i) {
                if (d.kind == Value::Kind::string && d.str == day_keys[i]) r.days |= 1u << i;
            }
        }
        r.from = *from;
        r.to = *to % day; // "24:00" is midnight
        r.mibps = mibps->number;
        if (valid(r)) rules.push_back(r);
    }
    return rules;
}

} // namespace v023

} // namespace

TEST_CASE("a rule limits the speed on its days between its times") {
    const std::vector<Rule> rules{down_rule(weekdays, 18 * 60, 0, 5)}; // weekdays 18:00-00:00
    CHECK(down_at(rules, at(Mon, 18)) == 5.0);
    CHECK(down_at(rules, at(Mon, 23, 59)) == 5.0);
    CHECK(down_at(rules, at(Fri, 20)) == 5.0);
    CHECK_FALSE(down_at(rules, at(Mon, 17, 59)).has_value());
    CHECK_FALSE(down_at(rules, at(Tue, 0)).has_value());
    CHECK_FALSE(down_at(rules, at(Sat, 19)).has_value());
    CHECK_FALSE(down_at({}, at(Mon, 18)).has_value());
}

TEST_CASE("a rule past midnight runs into the next day, from the days it starts on") {
    const std::vector<Rule> friday_night{down_rule(fri, 22 * 60, 6 * 60, 2)};
    CHECK(down_at(friday_night, at(Fri, 22)) == 2.0);
    CHECK(down_at(friday_night, at(Sat, 5, 59)) == 2.0);
    CHECK_FALSE(down_at(friday_night, at(Sat, 6)).has_value());
    CHECK_FALSE(down_at(friday_night, at(Fri, 5)).has_value()); // Thursday night: not chosen
    CHECK_FALSE(down_at(friday_night, at(Thu, 23)).has_value());

    const std::vector<Rule> sunday_night{down_rule(sun, 23 * 60, 60, 1)}; // across the end of the week
    CHECK(down_at(sunday_night, at(Sun, 23, 30)) == 1.0);
    CHECK(down_at(sunday_night, at(Mon, 0, 30)) == 1.0);
    CHECK_FALSE(down_at(sunday_night, at(Mon, 1)).has_value());
}

TEST_CASE("equal times are a whole day") {
    const std::vector<Rule> saturday{down_rule(sat, 0, 0, 8)};
    CHECK(down_at(saturday, at(Sat, 0)) == 8.0);
    CHECK(down_at(saturday, at(Sat, 23, 59)) == 8.0);
    CHECK_FALSE(down_at(saturday, at(Sun, 0)).has_value());
    CHECK_FALSE(down_at(saturday, at(Fri, 23, 59)).has_value());

    const std::vector<Rule> from_eight{down_rule(sat, 8 * 60, 8 * 60, 8)};
    CHECK(down_at(from_eight, at(Sun, 7, 59)) == 8.0);
    CHECK_FALSE(down_at(from_eight, at(Sun, 8)).has_value());
}

TEST_CASE("where rules overlap, the lowest speed wins") {
    const std::vector<Rule> rules{down_rule(every_day, 18 * 60, 0, 5), down_rule(sat | sun, 10 * 60, 0, 8)};
    CHECK(down_at(rules, at(Sat, 11)) == 8.0);
    CHECK(down_at(rules, at(Sat, 19)) == 5.0);
    CHECK(down_at(rules, at(Mon, 19)) == 5.0);
}

TEST_CASE("the schedule's next change and the limit after it") {
    const std::vector<Rule> rules{down_rule(weekdays, 18 * 60, 0, 5)};
    auto next = next_down(rules, at(Mon, 12));
    REQUIRE(next.has_value());
    CHECK(next->minutes == 6 * 60);
    CHECK(next->limit == 5.0);

    next = next_down(rules, at(Mon, 18));
    REQUIRE(next.has_value());
    CHECK(next->minutes == 6 * 60);
    CHECK_FALSE(next->limit.has_value());

    next = next_down(rules, at(Sat, 12)); // the weekend: Monday 18:00
    REQUIRE(next.has_value());
    CHECK(next->minutes == 54 * 60);

    // Back-to-back rules with the same speed are one stretch.
    const std::vector<Rule> joined{down_rule(mon, 18 * 60, 0, 5), down_rule(tue, 0, 6 * 60, 5)};
    next = next_down(joined, at(Mon, 19));
    REQUIRE(next.has_value());
    CHECK(next->minutes == 11 * 60);

    CHECK_FALSE(next_down({}, at(Mon, 12)).has_value());
    const std::vector<Rule> always{down_rule(every_day, 0, 0, 3)};
    CHECK(down_at(always, at(Thu, 3)) == 3.0);
    CHECK_FALSE(next_down(always, at(Thu, 3)).has_value());
}

TEST_CASE("schedule: each direction has its own speeds, changes and rules") {
    // Weekdays 18-24: downloads 5, uploads 1; weekends all day: uploads 2 only; Mondays 20-22:
    // downloads 3 only.
    const std::vector<Rule> rules{
        Rule{.days = weekdays, .from = 18 * 60, .to = 0, .down = 5, .up = 1},
        Rule{.days = sat | sun, .from = 0, .to = 0, .down = std::nullopt, .up = 2},
        Rule{.days = mon, .from = 20 * 60, .to = 22 * 60, .down = 3, .up = std::nullopt},
    };
    CHECK(schedule::limit_at(rules, at(Mon, 19), Dir::down) == 5.0);
    CHECK(schedule::limit_at(rules, at(Mon, 19), Dir::up) == 1.0);
    CHECK(schedule::limit_at(rules, at(Mon, 21), Dir::down) == 3.0); // the lower one
    CHECK(schedule::limit_at(rules, at(Mon, 21), Dir::up) == 1.0);   // the ↓-only rule leaves ↑ alone
    CHECK_FALSE(schedule::limit_at(rules, at(Sat, 12), Dir::down).has_value()); // the ↑-only rule leaves ↓ alone
    CHECK(schedule::limit_at(rules, at(Sat, 12), Dir::up) == 2.0);
    CHECK_FALSE(schedule::limit_at(rules, at(Tue, 12), Dir::up).has_value());

    // Each direction's next change from its own rules: Monday 19:00 -> ↓ 3 at 20:00, ↑ none at 24:00.
    auto down = schedule::next_change(rules, at(Mon, 19), Dir::down);
    auto up = schedule::next_change(rules, at(Mon, 19), Dir::up);
    REQUIRE(down.has_value());
    REQUIRE(up.has_value());
    CHECK(down->minutes == 60);
    CHECK(down->limit == 3.0);
    CHECK(up->minutes == 5 * 60);
    CHECK_FALSE(up->limit.has_value());
    // Friday 23:00: ↓ ends at 24:00; ↑ goes from 1 to the weekend's 2 then.
    down = schedule::next_change(rules, at(Fri, 23), Dir::down);
    up = schedule::next_change(rules, at(Fri, 23), Dir::up);
    REQUIRE(down.has_value());
    REQUIRE(up.has_value());
    CHECK(down->minutes == 60);
    CHECK_FALSE(down->limit.has_value());
    CHECK(up->minutes == 60);
    CHECK(up->limit == 2.0);

    CHECK(schedule::limits(rules, Dir::down));
    CHECK(schedule::limits(rules, Dir::up));
    const std::vector<Rule> only_up{rules[1]};
    CHECK_FALSE(schedule::limits(only_up, Dir::down));
    CHECK_FALSE(schedule::next_change(only_up, at(Sat, 12), Dir::down).has_value());
    CHECK(schedule::limits(only_up, Dir::up));
    CHECK_FALSE(schedule::limits({}, Dir::up));

    // A rule needs a speed for at least one direction, each in range.
    CHECK_FALSE(schedule::valid(Rule{.days = mon, .from = 0, .to = 60, .down = std::nullopt, .up = std::nullopt}));
    CHECK_FALSE(schedule::valid(Rule{.days = mon, .from = 0, .to = 60, .down = 5, .up = 0}));
    CHECK_FALSE(schedule::valid(Rule{.days = mon, .from = 0, .to = 60, .down = 20000, .up = 1}));
    CHECK(schedule::valid(Rule{.days = mon, .from = 0, .to = 60, .down = std::nullopt, .up = 0.1}));
    CHECK(schedule::speed_of(rules[0], Dir::up) == 1.0);
    CHECK_FALSE(schedule::speed_of(rules[1], Dir::down).has_value());
}

TEST_CASE("when a change happens, as \"until ...\" shows it") {
    CHECK(schedule::when_text(at(Mon, 20), 60) == "21:00");
    CHECK(schedule::when_text(at(Mon, 20), 4 * 60) == "24:00");
    CHECK(schedule::when_text(at(Mon, 20), 10 * 60) == "Tue 06:00");
    CHECK(schedule::when_text(at(Sun, 23), 60) == "24:00");
    CHECK(schedule::when_text(at(Sun, 23), 2 * 60) == "Mon 01:00");
}

TEST_CASE("times and speeds as text") {
    CHECK(schedule::parse_time("18:00") == 18 * 60);
    CHECK(schedule::parse_time("8:30") == 8 * 60 + 30);
    CHECK(schedule::parse_time("00:00") == 0);
    CHECK(schedule::parse_time("24:00") == 24 * 60);
    for (const char* bad : {"", "24:01", "7:60", "1800", "18:0", ":30", "ab:cd", "123:00", "18:00 ", "-1:00"}) {
        CAPTURE(bad);
        CHECK_FALSE(schedule::parse_time(bad).has_value());
    }
    CHECK(schedule::time_text(8 * 60 + 30) == "08:30");
    CHECK(schedule::time_text(0) == "00:00");
    CHECK(schedule::speed_text(5) == "5 MiB/s");
    CHECK(schedule::speed_text(2.5) == "2.5 MiB/s");
    CHECK(schedule::speed_text(10000) == "10000 MiB/s");
    CHECK(schedule::bytes_per_s(2.5) == 5u * 1024 * 1024 / 2);
}

TEST_CASE("rules as JSON; invalid ones are dropped") {
    const std::vector<Rule> rules{down_rule(weekdays, 18 * 60, 0, 5), down_rule(sat | sun, 22 * 60, 6 * 60, 2.5)};
    CHECK(json::stringify(schedule::to_json({rules[0]})) ==
          R"([{"days":["mon","tue","wed","thu","fri"],"from":"18:00","to":"00:00","mibps":5}])");
    auto parsed = json::parse(json::stringify(schedule::to_json(rules)));
    REQUIRE(parsed.has_value());
    CHECK(schedule::from_json(*parsed) == rules);

    auto mixed = json::parse(R"([
        {"days":["sat"],"from":"10:00","to":"24:00","mibps":8},
        {"days":[],"from":"10:00","to":"12:00","mibps":8},
        {"days":["someday"],"from":"10:00","to":"12:00","mibps":8},
        {"days":["mon"],"from":"25:00","to":"12:00","mibps":8},
        {"days":["mon"],"from":"10:00","mibps":8},
        {"days":["mon"],"from":"10:00","to":"12:00","mibps":0},
        {"days":["mon"],"from":"10:00","to":"12:00","mibps":"8"},
        {"days":["mon"],"from":"10:00","to":"12:00"},
        {"days":["mon"],"from":"10:00","to":"12:00","mibps":8,"upMiBps":"2"},
        {"days":["mon"],"from":"10:00","to":"12:00","mibps":8,"upMiBps":0},
        {"days":["mon"],"from":"10:00","to":"12:00","upMiBps":20000},
        "junk"
    ])");
    REQUIRE(mixed.has_value());
    const auto kept = schedule::from_json(*mixed);
    REQUIRE(kept.size() == 1);
    CHECK(kept[0] == down_rule(sat, 10 * 60, 0, 8)); // "24:00" is midnight
    CHECK(schedule::from_json(json::Value::make_object()).empty());

    std::vector<Rule> many(30, rules[0]);
    auto long_list = json::parse(json::stringify(schedule::to_json(many)));
    REQUIRE(long_list.has_value());
    CHECK(schedule::from_json(*long_list).size() == schedule::max_rules);
}

TEST_CASE("schedule: rules with upload speeds as JSON, and as grab 0.23 reads them") {
    const std::vector<Rule> rules{
        Rule{.days = weekdays, .from = 18 * 60, .to = 0, .down = 5, .up = 1.5},    // both
        Rule{.days = sat | sun, .from = 0, .to = 0, .down = std::nullopt, .up = 2}, // uploads only
        down_rule(mon, 20 * 60, 22 * 60, 3),                                      // downloads only
    };
    const auto text = json::stringify(schedule::to_json(rules));
    CHECK(text == R"([{"days":["mon","tue","wed","thu","fri"],"from":"18:00","to":"00:00","mibps":5,"upMiBps":1.5},)"
                  R"({"days":["sat","sun"],"from":"00:00","to":"00:00","upMiBps":2},)"
                  R"({"days":["mon"],"from":"20:00","to":"22:00","mibps":3}])");
    auto parsed = json::parse(text);
    REQUIRE(parsed.has_value());
    CHECK(schedule::from_json(*parsed) == rules);

    // grab 0.23 written rules: "mibps" only, read as download speeds.
    auto old = json::parse(R"([{"days":["sat"],"from":"10:00","to":"12:00","mibps":8}])");
    REQUIRE(old.has_value());
    CHECK(schedule::from_json(*old) == std::vector<Rule>{down_rule(sat, 10 * 60, 12 * 60, 8)});
    // A speed of null is no speed (that direction not limited by the rule).
    auto nulls = json::parse(R"([{"days":["sat"],"from":"10:00","to":"12:00","mibps":null,"upMiBps":4}])");
    REQUIRE(nulls.has_value());
    CHECK(schedule::from_json(*nulls) ==
          std::vector<Rule>{Rule{.days = sat, .from = 10 * 60, .to = 12 * 60, .down = std::nullopt, .up = 4}});

    // What grab 0.23 makes of them: the download speeds, as it would have saved them; the rule with
    // only an upload speed is dropped, not read as a download limit (nor anything thrown).
    const auto older = v023::from_json(*parsed);
    CHECK(older == std::vector<v023::Rule>{{weekdays, 18 * 60, 0, 5}, {mon, 20 * 60, 22 * 60, 3}});
    // Only upload speeds at all: no rules for it, so no download limit at those times.
    auto up_only = json::parse(json::stringify(schedule::to_json({rules[1]})));
    REQUIRE(up_only.has_value());
    CHECK(v023::from_json(*up_only).empty());
    // Rules of downloads only are written exactly as grab 0.23 wrote them.
    const std::vector<Rule> down_only{rules[2]};
    auto same = json::parse(json::stringify(schedule::to_json(down_only)));
    REQUIRE(same.has_value());
    CHECK(v023::from_json(*same) == std::vector<v023::Rule>{{mon, 20 * 60, 22 * 60, 3}});
}

TEST_CASE("a change made during the schedule holds until its next change") {
    const schedule::Override change{{false, 5}, 3, 1000}; // limit switched off while 3 MiB/s was scheduled
    CHECK(schedule::holds(change, 3.0, 999));
    CHECK_FALSE(schedule::holds(change, 3.0, 1000));        // its time is up
    CHECK_FALSE(schedule::holds(change, 2.0, 999));         // the schedule moved on to another limit
    CHECK_FALSE(schedule::holds(change, std::nullopt, 999)); // or to none

    const schedule::Limit own{true, 7};
    auto a = schedule::apply(own, 3.0, change, 999);
    CHECK(a.source == schedule::Source::change);
    CHECK(a.limit == schedule::Limit{false, 5});
    a = schedule::apply(own, 3.0, change, 1000);
    CHECK(a.source == schedule::Source::schedule);
    CHECK(a.limit == schedule::Limit{true, 3});
    a = schedule::apply(own, std::nullopt, change, 999);
    CHECK(a.source == schedule::Source::own);
    CHECK(a.limit == own);
    a = schedule::apply(own, std::nullopt, std::nullopt, 999);
    CHECK(a.source == schedule::Source::own);
}

TEST_CASE("the local time of the week") {
    const auto now = schedule::local_now();
    CHECK(now.minute >= 0);
    CHECK(now.minute < schedule::week);
    CHECK(now.second >= 0);
    CHECK(now.second < 60);
    CHECK(now.unix > 1'700'000'000);
}

TEST_CASE("schedule: a follower wakes just past the next whole second") {
    const auto wait = schedule::to_next_second();
    CHECK(wait > std::chrono::milliseconds(0));
    CHECK(wait <= std::chrono::milliseconds(1010));
    const auto woke = std::chrono::system_clock::now() + wait;
    const auto into = std::chrono::duration_cast<std::chrono::milliseconds>(woke.time_since_epoch()) % std::chrono::seconds(1);
    CHECK(into >= std::chrono::milliseconds(5)); // past the second, not just before it
    CHECK(into <= std::chrono::milliseconds(60));
}
