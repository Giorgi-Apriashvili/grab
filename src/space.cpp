#include "space.hpp"

#include "fetch.hpp"
#include "util.hpp"

#include <algorithm>
#include <charconv>
#include <format>
#include <system_error>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)
#endif

namespace grab::space {

namespace fs = std::filesystem;

namespace {

// `path` or the nearest folder above it that exists; nullopt when none does (a missing drive).
std::optional<fs::path> nearest_existing(fs::path path) {
    std::error_code ec;
    path = fs::absolute(path, ec);
    if (ec) return std::nullopt;
    while (true) {
        if (fs::is_directory(path, ec)) return path;
        const auto up = path.parent_path();
        if (up.empty() || up == path) return std::nullopt;
        path = up;
    }
}

} // namespace

std::string drive_of(const fs::path& path) {
    std::string drive;
#ifdef _WIN32
    if (const auto at = nearest_existing(path)) {
        wchar_t volume[MAX_PATH + 1]{};
        if (GetVolumePathNameW(at->c_str(), volume, MAX_PATH + 1)) drive = util::path_to_utf8(fs::path(volume));
    }
#endif
    if (drive.empty()) {
        std::error_code ec;
        const auto full = fs::absolute(path, ec);
        drive = util::path_to_utf8((ec ? path : full).root_path());
    }
    // "E:\" reads "E:", "\\server\share\" "\\server\share"; "/" stays.
    while (drive.size() > 1 && (drive.ends_with('\\') || drive.ends_with('/'))) drive.pop_back();
    return drive;
}

std::optional<std::uint64_t> free_bytes(const fs::path& path) {
    const auto at = nearest_existing(path);
    if (!at) return std::nullopt;
    std::error_code ec;
    const auto info = fs::space(*at, ec);
    if (ec) return std::nullopt;
    std::uint64_t free = info.available;
#ifndef NDEBUG
    // Testing a full drive without filling one.
    if (const auto cap = util::getenv_utf8("GRAB_TEST_FREE_BYTES")) {
        std::uint64_t n = 0;
        if (const auto [end, err] = std::from_chars(cap->data(), cap->data() + cap->size(), n);
            err == std::errc{} && end == cap->data() + cap->size()) {
            free = std::min(free, n);
        }
    }
#endif
    return free;
}

std::uint64_t still_needed(const fs::path& target, std::string_view remote_path, std::uint64_t size) {
    if (const auto partial = fetch::saved_progress(target, remote_path); partial && partial->second == size) {
        return size > partial->first ? size - partial->first : 0;
    }
    std::error_code ec;
    if (fs::is_regular_file(target, ec) && fs::file_size(target, ec) == size && !ec) return 0;
    return size;
}

std::string shortage(const std::string& drive, std::uint64_t needed, std::uint64_t free) {
    return std::format("not enough free space on {} ({} needed, {} free)", drive, util::format_size(needed),
                       util::format_size(free));
}

} // namespace grab::space
