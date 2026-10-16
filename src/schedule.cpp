#include "schedule.hpp"

#include "rate.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <ctime>
#include <format>
#include <set>

namespace grab::schedule {

namespace {

using json::Value;

constexpr std::array<const char*, 7> day_keys{"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
constexpr std::array<const char*, 7> day_names{"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};

int wrap(int minute) { return ((minute % week) + week) % week; }

int length_of(const Rule& r) { return r.to > r.from ? r.to - r.from : r.to + day - r.from; }

// Where the rules start and end on the week, each point once, in order from Monday 00:00.
std::vector<int> boundaries(const std::vector<Rule>& rules) {
    std::set<int> points;
    for (const auto& r : rules) {
        if (!valid(r)) continue;
        for (int d = 0; d < 7; ++d) {
            if ((r.days & (1u << d)) == 0) continue;
            const int start = d * day + r.from;
            points.insert(wrap(start));
            points.insert(wrap(start + length_of(r)));
        }
    }
    return {points.begin(), points.end()};
}

} // namespace

bool valid(const Rule& r) {
    return r.days != 0 && (r.days & ~0x7Fu) == 0 && r.from >= 0 && r.from < day && r.to >= 0 && r.to < day &&
           r.mibps >= min_mibps && r.mibps <= max_mibps; // also false for NaN
}

std::optional<double> limit_at(const std::vector<Rule>& rules, int minute) {
    minute = wrap(minute);
    std::optional<double> lowest;
    for (const auto& r : rules) {
        if (!valid(r)) continue;
        const int length = length_of(r);
        for (int d = 0; d < 7; ++d) {
            if ((r.days & (1u << d)) != 0 && wrap(minute - (d * day + r.from)) < length) {
                lowest = std::min(lowest.value_or(r.mibps), r.mibps);
                break;
            }
        }
    }
    return lowest;
}

std::optional<Change> next_change(const std::vector<Rule>& rules, int minute) {
    minute = wrap(minute);
    const auto now = limit_at(rules, minute);
    std::vector<int> ahead;
    for (const int b : boundaries(rules)) ahead.push_back(wrap(b - minute) == 0 ? week : wrap(b - minute));
    std::ranges::sort(ahead);
    for (const int a : ahead) {
        if (a == week) break; // a week on is now again
        if (auto then = limit_at(rules, minute + a); then != now) return Change{a, then};
    }
    return std::nullopt;
}

std::string rclone_bwlimit(const std::vector<Rule>& rules) {
    const auto points = boundaries(rules);
    if (points.empty()) return "";
    auto rate_text = [](std::optional<double> mibps) {
        return mibps ? rate::bwlimit_arg(bytes_per_s(*mibps)) : std::string("off");
    };
    // The limit is the same from one boundary to the next: an entry wherever it changes.
    std::string out;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const auto limit = limit_at(rules, points[i]);
        const auto before = limit_at(rules, points[(i + points.size() - 1) % points.size()]);
        if (limit == before) continue;
        if (!out.empty()) out += ' ';
        out += std::format("{}-{},{}", day_names[static_cast<std::size_t>(points[i] / day)], time_text(points[i] % day),
                           rate_text(limit));
    }
    return out.empty() ? rate_text(limit_at(rules, 0)) : out; // one limit all week
}

std::uint64_t bytes_per_s(double mibps) { return static_cast<std::uint64_t>(mibps * 1024 * 1024); }

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

std::string time_text(int minutes) { return std::format("{:02}:{:02}", minutes / 60, minutes % 60); }

std::string when_text(int minute, int minutes_ahead) {
    minute = wrap(minute);
    const int at = minute + minutes_ahead;
    const int midnight = (minute / day + 1) * day; // the end of `minute`'s day
    if (at < midnight) return time_text(at % day);
    if (at == midnight) return "24:00";
    return std::format("{} {}", day_names[static_cast<std::size_t>((at / day) % 7)], time_text(at % day));
}

std::string speed_text(double mibps) { return std::format("{:g} MiB/s", std::round(mibps * 10) / 10); }

Value to_json(const std::vector<Rule>& rules) {
    Value list = Value::make_array();
    for (const auto& r : rules) {
        Value days = Value::make_array();
        for (std::size_t d = 0; d < 7; ++d) {
            if ((r.days & (1u << d)) != 0) days.push(Value::make_string(day_keys[d]));
        }
        Value v = Value::make_object();
        v.set("days", std::move(days));
        v.set("from", Value::make_string(time_text(r.from)));
        v.set("to", Value::make_string(time_text(r.to)));
        v.set("mibps", Value::make_number(r.mibps));
        list.push(std::move(v));
    }
    return list;
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

bool holds(const Override& o, std::optional<double> scheduled, std::int64_t now) {
    return scheduled == o.scheduled && now < o.until;
}

Applied apply(const Limit& own, std::optional<double> scheduled, const std::optional<Override>& change,
              std::int64_t now) {
    if (change && holds(*change, scheduled, now)) return {change->limit, Source::change};
    if (scheduled) return {Limit{true, *scheduled}, Source::schedule};
    return {own, Source::own};
}

Now local_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    Now n;
    n.minute = (tm.tm_wday + 6) % 7 * day + tm.tm_hour * 60 + tm.tm_min; // tm_wday 0 is Sunday
    n.second = std::min(tm.tm_sec, 59);                                // 60 in a leap second
    n.unix = static_cast<std::int64_t>(t);
    return n;
}

} // namespace grab::schedule
