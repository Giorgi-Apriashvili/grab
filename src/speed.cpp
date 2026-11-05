#include "speed.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace grab::speed {

namespace {

double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

} // namespace

void Meter::sample(Clock::time_point now, std::uint64_t total) {
    if (at_ && now <= *at_) return;
    const std::uint64_t came = at_ && total > total_ ? total - total_ : 0;
    if (!at_ || (steps_.empty() && came == 0)) { // no bytes yet: it starts from here
        at_ = now;
        total_ = total;
        return;
    }
    if (steps_.empty()) since_ = *at_;
    steps_.push_back(Step{.end = now, .took = now - *at_, .bytes = came});
    at_ = now;
    total_ = total;
    // A step's age is its middle's: gone once that is meter_gone old.
    auto age = [&](const Step& s) { return now - (s.end - s.took / 2); };
    while (!steps_.empty() && age(steps_.front()) >= meter_gone) steps_.pop_front();
    if (std::ranges::all_of(steps_, [](const Step& s) { return s.bytes == 0; })) {
        steps_.clear(); // none in its window: it starts again at the next bytes
        since_.reset();
        rate_ = 0;
        return;
    }
    double bytes = 0;
    double time = 0;
    double span = 0;
    for (const auto& s : steps_) {
        const auto a = age(s);
        const double w = a <= meter_full ? 1.0 : seconds(meter_gone - a) / seconds(meter_gone - meter_full);
        bytes += w * static_cast<double>(s.bytes);
        time += w * seconds(s.took);
        span += seconds(s.took);
    }
    rate_ = span >= seconds(meter_least) && time > 0 ? bytes / time : 0;
}

void Pace::sample(Clock::time_point now, std::uint64_t files, std::uint64_t bytes) {
    if (!points_.empty() && now - points_.back().at < pace_step) return;
    points_.push_back(Point{.at = now, .files = files, .bytes = bytes});
    // The window: the points of its last pace_window, the first of them its start.
    while (points_.size() > 2 && now - points_[1].at >= pace_window) points_.pop_front();
}

std::optional<Pace::Model> Pace::model() const {
    if (points_.size() < 2) return std::nullopt;
    const double total = seconds(points_.back().at - points_.front().at);
    if (total < seconds(pace_least)) return std::nullopt;
    // Each second: how long, and the files and bytes that came in it (a count that went down
    // counts as none).
    struct Second {
        double t = 0;
        double files = 0;
        double bytes = 0;
    };
    std::vector<Second> secs;
    secs.reserve(points_.size() - 1);
    double files = 0;
    double bytes = 0;
    for (std::size_t k = 1; k < points_.size(); ++k) {
        const auto& a = points_[k - 1];
        const auto& b = points_[k];
        Second s{.t = seconds(b.at - a.at),
                 .files = b.files > a.files ? static_cast<double>(b.files - a.files) : 0.0,
                 .bytes = b.bytes > a.bytes ? static_cast<double>(b.bytes - a.bytes) : 0.0};
        files += s.files;
        bytes += s.bytes;
        secs.push_back(s);
    }
    if (files == 0 && bytes == 0) return std::nullopt; // nothing came: no pace to go by
    const double rate = bytes / total;
    if (files == 0) return Model{.per_file = 0, .per_byte = total / bytes, .rate = rate};
    if (bytes == 0) return Model{.per_file = total / files, .per_byte = 0, .rate = 0};
    double num = 0;
    double den = 0;
    double sq = 0;
    double fastest = total / bytes; // the least a byte cost in any second (at most the window's)
    for (const auto& s : secs) {
        const double u = total * s.files / files;
        const double v = total * s.bytes / bytes;
        num += (s.t - v) * (u - v);
        den += (u - v) * (u - v);
        sq += s.t * s.t;
        if (s.bytes > 0) fastest = std::min(fastest, s.t / s.bytes);
    }
    const double theta = std::clamp(num / (den + pace_ridge * sq), 0.0, 1.0);
    // Both are at most total / bytes, so per_file stays >= 0 and the window's own time exact.
    const double per_byte = std::max((1 - theta) * total / bytes, fastest);
    return Model{.per_file = std::max(0.0, (total - per_byte * bytes) / files), .per_byte = per_byte, .rate = rate};
}

std::optional<double> Pace::time_left(std::uint64_t files, std::uint64_t bytes) const {
    const auto m = model();
    if (!m) return std::nullopt;
    return m->per_file * static_cast<double>(files) + m->per_byte * static_cast<double>(bytes);
}

std::optional<double> Pace::time_left(std::uint64_t files, std::uint64_t bytes, double rate) const {
    const auto m = model();
    if (!m || !(rate > 0)) return std::nullopt;
    const double scale = m->rate > 0 ? m->rate / rate : 1.0;
    return scale * (m->per_file * static_cast<double>(files) + m->per_byte * static_cast<double>(bytes));
}

std::optional<double> Countdown::next(Clock::time_point now, std::optional<double> estimate) {
    if (!estimate || !std::isfinite(*estimate)) {
        shown_.reset();
        return std::nullopt;
    }
    const double e = std::max(0.0, *estimate);
    if (!shown_) {
        shown_ = e;
        at_ = now;
        return shown_;
    }
    const double dt = std::max(0.0, seconds(now - at_));
    const double counted = std::max(0.0, *shown_ - dt);
    const double part = 1 - std::exp(-dt / seconds(countdown_tau));
    shown_ = std::clamp(counted + part * (e - counted), e / countdown_band, e * countdown_band);
    at_ = now;
    return shown_;
}

std::optional<double> file_left(std::optional<Clock::time_point> since, Clock::time_point now,
                                std::uint64_t bytes_left, double rate) {
    if (!since || now - *since < pace_least || !(rate > 0) || bytes_left == 0) return std::nullopt;
    return static_cast<double>(bytes_left) / rate;
}

std::uint64_t shown(double bytes_per_s) {
    if (!(bytes_per_s > 0) || !std::isfinite(bytes_per_s)) return 0;
    if (bytes_per_s < 1024) return static_cast<std::uint64_t>(std::llround(bytes_per_s));
    double unit = 1024;
    while (bytes_per_s / unit >= 1024 && unit < 1024.0 * 1024 * 1024 * 1024 * 1024) unit *= 1024;
    const double tenths = std::round(bytes_per_s / unit * 10) / 10;
    return static_cast<std::uint64_t>(std::llround(tenths * unit));
}

} // namespace grab::speed
