#include "activity.hpp"

#include <algorithm>
#include <cmath>

namespace grab::activity {

int blend(std::optional<int> before, int now_ms) {
    now_ms = std::clamp(now_ms, min_ms, max_ms);
    if (!before || *before < min_ms || *before > max_ms) return now_ms;
    return static_cast<int>(std::lround(0.6 * now_ms + 0.4 * *before));
}

int guess(std::string_view key) {
    const auto kind = key.substr(0, key.find(':'));
    if (kind == "lsjson") return 2000;
    if (kind == "search") return 5000;
    if (kind == "list") return 10000;
    if (kind == "serve") return 2000;
    if (kind == "update") return 2000;
    return 3000;
}

void Records::feed(std::string_view chunk) {
    if (first_ == '\0') {
        count_ += static_cast<std::size_t>(std::ranges::count_if(chunk, [](char c) { return c == '\n' || c == '\0'; }));
        return;
    }
    for (const char c : chunk) {
        if (line_start_ && c == first_) ++count_;
        line_start_ = c == '\n';
    }
}

} // namespace grab::activity
