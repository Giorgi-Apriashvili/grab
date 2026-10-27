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
