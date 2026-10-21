#include "existing.hpp"

#include "local_name.hpp"
#include "rclone.hpp"
#include "util.hpp"

#include <chrono>
#include <format>
#include <set>
#include <system_error>

namespace grab::existing {

namespace fs = std::filesystem;

std::string_view name(Choice c) {
    switch (c) {
    case Choice::replace: return "replace";
    case Choice::keep_both: return "both";
    case Choice::skip: return "skip";
    case Choice::ask: break;
    }
    return "ask";
}

std::optional<Choice> parse(std::string_view text) {
    for (const auto c : {Choice::ask, Choice::replace, Choice::keep_both, Choice::skip}) {
        if (text == name(c)) return c;
    }
    return std::nullopt;
}

namespace {

// Size and modification time (nanoseconds after 1970) of a regular file.
std::optional<std::pair<std::uint64_t, std::int64_t>> stat(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return std::nullopt;
    const auto size = fs::file_size(p, ec);
    if (ec) return std::nullopt;
    const auto written = fs::last_write_time(p, ec);
    if (ec) return std::nullopt;
    const auto since_1970 = std::chrono::clock_cast<std::chrono::system_clock>(written).time_since_epoch();
    return std::pair{size, std::chrono::duration_cast<std::chrono::nanoseconds>(since_1970).count()};
}

} // namespace

bool differs(const fs::path& local, std::uint64_t size, std::optional<std::int64_t> modtime_ns) {
    const auto s = stat(local);
    return s && !fetch::up_to_date(s->first, s->second, size, modtime_ns);
}

bool rclone_replaces(const fs::path& local, std::uint64_t size, std::optional<std::int64_t> modtime_ns) {
    const auto s = stat(local);
    if (!s) return false;
    if (s->first != size) return true;
    constexpr std::int64_t window = 1'000'000'000; // rclone's modify window over SFTP
    return modtime_ns && (s->second > *modtime_ns + window || s->second < *modtime_ns - window);
}

std::string numbered(std::string_view name, int n) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos || dot == 0) return std::format("{} ({})", name, n);
    return std::format("{} ({}){}", name.substr(0, dot), n, name.substr(dot));
}

std::optional<fs::path> second_copy(const fs::path& target, const fetch::Reserved& reserved) {
    const std::string base = util::path_to_utf8(target.filename());
    for (int n = 2; n < 1000; ++n) {
        const auto p = target.parent_path() / util::path_from_utf8(numbered(base, n));
        std::error_code ec;
        if ((reserved && reserved(p)) || fs::exists(fs::symlink_status(p, ec))) continue;
        return p;
    }
    return std::nullopt;
}

std::optional<fs::path> kept_copy(const fs::path& target, std::uint64_t size, std::optional<std::int64_t> modtime_ns) {
    const std::string base = util::path_to_utf8(target.filename());
    for (int n = 2; n < 1000; ++n) {
        const auto p = target.parent_path() / util::path_from_utf8(numbered(base, n));
        const auto s = stat(p);
        std::error_code ec;
        if (!s && !fs::exists(fs::symlink_status(p, ec))) return std::nullopt; // copies are numbered in a row
        if (s && fetch::up_to_date(s->first, s->second, size, modtime_ns)) return p;
    }
    return std::nullopt;
}

std::vector<fs::path> pick_targets(const fs::path& dest, const std::vector<std::string>& paths, bool folders) {
    std::vector<fs::path> out;
    std::set<fs::path::string_type> taken;
    for (const auto& path : paths) {
        auto target = dest / local::relative(remote_basename(path));
        const std::string name = util::path_to_utf8(target.filename());
        for (int n = 2; taken.contains(local::key(target)) && n < 1000; ++n) {
            target = dest / util::path_from_utf8(folders ? std::format("{} ({})", name, n) : numbered(name, n));
        }
        taken.insert(local::key(target));
        out.push_back(std::move(target));
    }
    return out;
}

std::optional<Local> local_file(const fs::path& p) {
    const auto s = stat(p);
    if (!s) return std::nullopt;
    return Local{s->first, s->second / 1'000'000'000};
}

} // namespace grab::existing
