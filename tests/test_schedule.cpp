#include "json.hpp"
#include "schedule.hpp"

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <vector>

using namespace grab;
using schedule::Rule;

namespace {

constexpr unsigned mon = 1, tue = 2, fri = 16, sat = 32, sun = 64;
constexpr unsigned weekdays = 0x1F, every_day = 0x7F;

// A minute of the week: day 0 = Monday.
int at(int weekday, int h, int m = 0) { return weekday * schedule::day + h * 60 + m; }
constexpr int Mon = 0, Tue = 1, Thu = 3, Fri = 4, Sat = 5, Sun = 6;

} // namespace

TEST_CASE("a rule limits the speed on its days between its times") {
    const std::vector<Rule> rules{{weekdays, 18 * 60, 0, 5}}; // weekdays 18:00-00:00
    CHECK(schedule::limit_at(rules, at(Mon, 18)) == 5.0);
    CHECK(schedule::limit_at(rules, at(Mon, 23, 59)) == 5.0);
    CHECK(schedule::limit_at(rules, at(Fri, 20)) == 5.0);
    CHECK_FALSE(schedule::limit_at(rules, at(Mon, 17, 59)).has_value());
    CHECK_FALSE(schedule::limit_at(rules, at(Tue, 0)).has_value());
    CHECK_FALSE(schedule::limit_at(rules, at(Sat, 19)).has_value());
    CHECK_FALSE(schedule::limit_at({}, at(Mon, 18)).has_value());
}

TEST_CASE("a rule past midnight runs into the next day, from the days it starts on") {
    const std::vector<Rule> friday_night{{fri, 22 * 60, 6 * 60, 2}};
    CHECK(schedule::limit_at(friday_night, at(Fri, 22)) == 2.0);
    CHECK(schedule::limit_at(friday_night, at(Sat, 5, 59)) == 2.0);
    CHECK_FALSE(schedule::limit_at(friday_night, at(Sat, 6)).has_value());
    CHECK_FALSE(schedule::limit_at(friday_night, at(Fri, 5)).has_value()); // Thursday night: not chosen
    CHECK_FALSE(schedule::limit_at(friday_night, at(Thu, 23)).has_value());

    const std::vector<Rule> sunday_night{{sun, 23 * 60, 60, 1}}; // across the end of the week
    CHECK(schedule::limit_at(sunday_night, at(Sun, 23, 30)) == 1.0);
    CHECK(schedule::limit_at(sunday_night, at(Mon, 0, 30)) == 1.0);
    CHECK_FALSE(schedule::limit_at(sunday_night, at(Mon, 1)).has_value());
}

TEST_CASE("equal times are a whole day") {
    const std::vector<Rule> saturday{{sat, 0, 0, 8}};
    CHECK(schedule::limit_at(saturday, at(Sat, 0)) == 8.0);
    CHECK(schedule::limit_at(saturday, at(Sat, 23, 59)) == 8.0);
    CHECK_FALSE(schedule::limit_at(saturday, at(Sun, 0)).has_value());
    CHECK_FALSE(schedule::limit_at(saturday, at(Fri, 23, 59)).has_value());

    const std::vector<Rule> from_eight{{sat, 8 * 60, 8 * 60, 8}};
    CHECK(schedule::limit_at(from_eight, at(Sun, 7, 59)) == 8.0);
    CHECK_FALSE(schedule::limit_at(from_eight, at(Sun, 8)).has_value());
}

TEST_CASE("where rules overlap, the lowest speed wins") {
    const std::vector<Rule> rules{{every_day, 18 * 60, 0, 5}, {sat | sun, 10 * 60, 0, 8}};
    CHECK(schedule::limit_at(rules, at(Sat, 11)) == 8.0);
    CHECK(schedule::limit_at(rules, at(Sat, 19)) == 5.0);
    CHECK(schedule::limit_at(rules, at(Mon, 19)) == 5.0);
}

TEST_CASE("the schedule's next change and the limit after it") {
    const std::vector<Rule> rules{{weekdays, 18 * 60, 0, 5}};
    auto next = schedule::next_change(rules, at(Mon, 12));
    REQUIRE(next.has_value());
    CHECK(next->minutes == 6 * 60);
    CHECK(next->limit == 5.0);

    next = schedule::next_change(rules, at(Mon, 18));
    REQUIRE(next.has_value());
    CHECK(next->minutes == 6 * 60);
    CHECK_FALSE(next->limit.has_value());

    next = schedule::next_change(rules, at(Sat, 12)); // the weekend: Monday 18:00
    REQUIRE(next.has_value());
    CHECK(next->minutes == 54 * 60);

    // Back-to-back rules with the same speed are one stretch.
    const std::vector<Rule> joined{{mon, 18 * 60, 0, 5}, {tue, 0, 6 * 60, 5}};
    next = schedule::next_change(joined, at(Mon, 19));
    REQUIRE(next.has_value());
    CHECK(next->minutes == 11 * 60);

    CHECK_FALSE(schedule::next_change({}, at(Mon, 12)).has_value());
    const std::vector<Rule> always{{every_day, 0, 0, 3}};
    CHECK(schedule::limit_at(always, at(Thu, 3)) == 3.0);
    CHECK_FALSE(schedule::next_change(always, at(Thu, 3)).has_value());
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

TEST_CASE("rclone gets the schedule as a weekly --bwlimit timetable") {
    CHECK(schedule::rclone_bwlimit({}).empty());
    CHECK(schedule::rclone_bwlimit({{mon, 18 * 60, 20 * 60, 5}, {mon, 20 * 60, 22 * 60, 2}}) ==
          "Mon-18:00,5120K Mon-20:00,2048K Mon-22:00,off");
    CHECK(schedule::rclone_bwlimit({{weekdays, 18 * 60, 0, 5}}) ==
          "Mon-18:00,5120K Tue-00:00,off Tue-18:00,5120K Wed-00:00,off Wed-18:00,5120K Thu-00:00,off "
          "Thu-18:00,5120K Fri-00:00,off Fri-18:00,5120K Sat-00:00,off");
    CHECK(schedule::rclone_bwlimit({{sun, 23 * 60, 60, 1}}) == "Mon-01:00,off Sun-23:00,1024K");
    CHECK(schedule::rclone_bwlimit({{every_day, 0, 0, 5}}) == "5120K"); // one limit all week
}

TEST_CASE("rules as JSON; invalid ones are dropped") {
    const std::vector<Rule> rules{{weekdays, 18 * 60, 0, 5}, {sat | sun, 22 * 60, 6 * 60, 2.5}};
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
        "junk"
    ])");
    REQUIRE(mixed.has_value());
    const auto kept = schedule::from_json(*mixed);
    REQUIRE(kept.size() == 1);
    CHECK(kept[0] == Rule{sat, 10 * 60, 0, 8}); // "24:00" is midnight
    CHECK(schedule::from_json(json::Value::make_object()).empty());

    std::vector<Rule> many(30, rules[0]);
    auto long_list = json::parse(json::stringify(schedule::to_json(many)));
    REQUIRE(long_list.has_value());
    CHECK(schedule::from_json(*long_list).size() == schedule::max_rules);
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
