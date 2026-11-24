#include "progress_line.hpp"

#include "util.hpp"

#include <cmath>
#include <format>

namespace grab::progress {

std::string duration(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) seconds = 0;
    const auto total = static_cast<std::uint64_t>(std::llround(seconds));
    const auto h = total / 3600;
    const auto m = total / 60 % 60;
    const auto s = total % 60;
    return h > 0 ? std::format("{}:{:02}:{:02}", h, m, s) : std::format("{}:{:02}", m, s);
}

namespace {

std::string speed_text(double bytes_per_second) {
    return util::format_size(static_cast<std::uint64_t>(bytes_per_second)) + "/s";
}

} // namespace

std::string line(const Status& s) {
    std::string out;
    if (s.files && s.files_done) out = std::format("{}/{} files  ", *s.files_done, *s.files);
    const int percent = s.total > 0 ? static_cast<int>(s.bytes * 100 / s.total) : 0;
    out += std::format("{} / {}  {}%", util::format_size(s.bytes), util::format_size(s.total), percent);
    if (s.speed > 0) out += "  " + speed_text(s.speed);
    out += std::format("  {} conn{}", s.connections, s.connections == 1 ? "" : "s");
    if (s.eta) out += "  ETA " + duration(*s.eta);
    return out;
}

void FolderMeter::sample(speed::Clock::time_point now, const Counts& c) {
    if (limit_changes_ && *limit_changes_ != c.limit_changes) {
        meter_.restart(now, c.received, static_cast<double>(c.limit));
        pace_.restart();
        countdown_.restart();
    }
    limit_changes_ = c.limit_changes;
    meter_.sample(now, c.received);
    speed_ = meter_.rate();
    const bool holding = meter_.holding();
    if (holding && !meter_.foreseen()) {
        // The new limit's speed doesn't show yet: the time left counts down on its own, then goes
        // to the first estimate from the new speed at once. A lower one than the speed then is
        // known at once: the time left from the limit (speed::Meter::ahead).
        eta_ = countdown_.hold(now);
        return;
    }
    // From its first bytes, as its speed: the time spent connecting isn't its pace (nor, after a
    // new limit, the second before its speed shows).
    if (meter_.since() && !holding) pace_.sample(now, c.finished, c.received);
    eta_ = countdown_.next(now, pace_.time_left(c.left_files, c.left_bytes, meter_.ahead()));
}

std::string done(std::uint64_t total, std::uint64_t fetched, double seconds, std::optional<std::size_t> files) {
    std::string out = "done: ";
    if (files) out += std::format("{} file{}, ", *files, *files == 1 ? "" : "s");
    out += std::format("{} in {}", util::format_size(total), duration(seconds));
    const bool timed = seconds >= 0.5 && fetched > 0;
    const std::string speed = timed ? speed_text(static_cast<double>(fetched) / seconds) : "";
    if (fetched > 0 && fetched < total) {
        out += std::format(" ({} of it now{})", util::format_size(fetched), timed ? ", " + speed : "");
    } else if (timed) {
        out += std::format(" ({})", speed);
    }
    return out;
}

std::string count(std::uint64_t n) {
    std::string digits = std::to_string(n);
    std::string out;
    out.reserve(digits.size() + digits.size() / 3);
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
        out += digits[i];
    }
    return out;
}

std::string took(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) seconds = 0;
    if (seconds < 9.95) return std::format("{:.1f} s", seconds);
    const auto total = static_cast<std::uint64_t>(std::llround(seconds));
    if (total < 60) return std::format("{} s", total);
    if (total < 600) return total % 60 == 0 ? std::format("{} min", total / 60) : std::format("{} min {} s", total / 60, total % 60);
    const auto minutes = (total + 30) / 60;
    if (minutes < 60) return std::format("{} min", minutes);
    return minutes % 60 == 0 ? std::format("{} h", minutes / 60) : std::format("{} h {} min", minutes / 60, minutes % 60);
}

std::string uploaded(std::size_t files, std::uint64_t total, std::uint64_t sent, std::string_view place, double seconds) {
    std::string out = std::format("Uploaded {} file{} ({}", count(files), files == 1 ? "" : "s", util::format_size(total));
    if (sent > 0 && sent < total) out += std::format(", {} of it now", util::format_size(sent));
    out += std::format(") to {} in {}", place, took(seconds));
    if (seconds >= 0.5 && sent > 0) out += ", " + speed_text(static_cast<double>(sent) / seconds);
    return out;
}

std::string folder_failed(std::string_view failed, std::string_view checked) {
    std::string out;
    if (!checked.empty()) out = std::format("  {}\n", checked);
    out += std::format("grab: error: {}\n", failed);
    out += "grab: the other files are on disk; run the same command again to retry\n";
    return out;
}

std::string fit(std::string_view text, std::size_t width) {
    if (width == 0) return {};
    // Count code points: continuation bytes (10xxxxxx) don't start a column.
    std::size_t columns = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++columns;
    }
    if (columns <= width) return std::string(text);
    std::string out;
    std::size_t kept = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const bool starts = (static_cast<unsigned char>(text[i]) & 0xC0) != 0x80;
        if (starts && kept == width - 1) break;
        if (starts) ++kept;
        out += text[i];
    }
    return out + "\xE2\x80\xA6"; // …
}

} // namespace grab::progress
