#pragma once

// The window's status bar shows what grab is doing while it takes a while (a listing, a search, a
// stream starting), with a progress bar: real progress where grab can count, else filled over how
// long the same step took before. These are the parts without the window: the remembered times
// and counting a listing's records as they stream in.

#include <cstddef>
#include <optional>
#include <string_view>

namespace grab::activity {

// A step's remembered time (milliseconds) after it took `now_ms` once more: a moving average
// leaning on the latest, so a server that got slower shows it soon.
[[nodiscard]] int blend(std::optional<int> before, int now_ms);
// The first guess for a step never timed, by its kind: the part of `key` before ':' ("lsjson",
// "search", "list", "update"; on engine = grab "readdir", "find", "walk", "list.find",
// "list.walk"). Play's warm part ("warm") shows real progress, so it keeps the general guess.
[[nodiscard]] int guess(std::string_view key);
// Remembered times outside this range are dropped as nonsense.
inline constexpr int min_ms = 1;
inline constexpr int max_ms = 30 * 60 * 1000;

// Counts the records (lines, or NUL-ended `find -print0` entries) in a listing's output as it
// arrives in chunks; with `first`, only those starting with it (rclone lsjson: an item a line,
// "{...}", between "[" and "]").
class Records {
public:
    explicit Records(char first = '\0') : first_(first) {}
    void feed(std::string_view chunk);
    [[nodiscard]] std::size_t count() const { return count_; }

private:
    char first_;
    bool line_start_ = true;
    std::size_t count_ = 0;
};

} // namespace grab::activity
