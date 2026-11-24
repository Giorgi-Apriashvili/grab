#include "local_walk.hpp"

#include "fetch.hpp"
#include "rclone_encoding.hpp"
#include "util.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <set>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)
#endif

namespace grab::local_walk {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view part_suffix = ".grabpart";
constexpr std::string_view record_suffix = ".grabpart.json";

// "2023-05-06T11:18:12.123456789Z"
std::string rfc3339_ns(std::int64_t ns) {
    return std::format("{:%FT%TZ}", std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{ns}});
}

std::string joined(std::string_view dir, std::string_view name) {
    return dir.empty() ? std::string(name) : std::format("{}/{}", dir, name);
}

// A record of grab's own partial download (fetch.hpp): a small file that parses as one.
bool is_record(const fs::path& p) {
    constexpr std::uintmax_t record_max = 1 << 20; // a record is a few KiB
    std::error_code ec;
    const auto size = fs::file_size(p, ec);
    if (ec || size > record_max) return false;
    const auto text = util::read_file(p);
    return text && fetch::state_from_json(*text).has_value();
}

// One name a folder holds.
struct Entry {
    fs::path name;       // as the file system has it
    std::string utf8;    // the same in UTF-8
    bool dir = false;
    bool link = false;   // a symbolic link or a junction: not followed
    std::uint64_t size = 0;
    std::int64_t mtime_ns = 0;
};

#ifdef _WIN32
std::int64_t unix_ns(const FILETIME& ft) {
    const auto ticks = static_cast<std::int64_t>((std::uint64_t{ft.dwHighDateTime} << 32) | ft.dwLowDateTime);
    return (ticks - 116444736000000000LL) * 100; // 100 ns since 1601
}

// The folder's path as Windows reads it past 260 characters while long paths are off: "\\?\C:\..."
// ("\\?\UNC\server\share\..." for a share), absolute and without "." or "..".
fs::path readable(const fs::path& p) {
    if (util::long_paths()) return p;
    std::error_code ec;
    fs::path abs = fs::absolute(p, ec);
    if (ec) return p;
    abs = abs.lexically_normal();
    abs.make_preferred();
    const std::wstring& s = abs.native();
    if (s.starts_with(LR"(\\?\)")) return abs;
    if (s.starts_with(LR"(\\)")) return fs::path(LR"(\\?\UNC\)" + s.substr(2));
    return fs::path(LR"(\\?\)" + s);
}

// The names in `dir`: a failure's text, or none.
std::expected<std::vector<Entry>, std::string> entries(const fs::path& dir) {
    std::vector<Entry> out;
    WIN32_FIND_DATAW fd{};
    const fs::path pattern = dir / L"*";
    const HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return util::fail(util::error_message(GetLastError()));
    DWORD last = ERROR_SUCCESS;
    do {
        if (const std::wstring_view name = fd.cFileName; name != L"." && name != L"..") {
            Entry e;
            e.name = fs::path(name);
            e.utf8 = util::to_utf8(name);
            // Symbolic links and junctions are name surrogates; other reparse points (a OneDrive
            // file not on this PC, a deduplicated one) are the files themselves.
            e.link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && IsReparseTagNameSurrogate(fd.dwReserved0);
            e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            e.size = (std::uint64_t{fd.nFileSizeHigh} << 32) | fd.nFileSizeLow;
            e.mtime_ns = unix_ns(fd.ftLastWriteTime);
            out.push_back(std::move(e));
        }
        if (!FindNextFileW(h, &fd)) last = GetLastError();
    } while (last == ERROR_SUCCESS);
    FindClose(h);
    if (last != ERROR_NO_MORE_FILES) return util::fail(util::error_message(last));
    return out;
}

bool utf8_round_trips(const Entry& e) { return util::to_wide(e.utf8) == e.name.native(); }

// A name ending in a dot or a space ("a.", made by WSL, Cygwin or a copy from a Samba share):
// Windows lists it, but opening it by its path opens the name without them ("a"), another file.
bool lost_on_open(const Entry& e) { return e.utf8.ends_with('.') || e.utf8.ends_with(' '); }
#else
fs::path readable(const fs::path& p) { return p; }

std::expected<std::vector<Entry>, std::string> entries(const fs::path& dir) {
    std::vector<Entry> out;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        Entry e;
        e.name = it->path().filename();
        e.utf8 = e.name.string();
        const auto st = it->symlink_status(ec);
        if (ec) break;
        e.link = fs::is_symlink(st);
        e.dir = fs::is_directory(st);
        if (!e.link && !e.dir) {
            e.size = it->file_size(ec);
            const auto t = it->last_write_time(ec);
            const auto since = std::chrono::clock_cast<std::chrono::system_clock>(t).time_since_epoch();
            e.mtime_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(since).count();
            ec.clear();
        }
        out.push_back(std::move(e));
    }
    if (ec) return util::fail(ec.message());
    return out;
}

bool utf8_round_trips(const Entry&) { return true; }
bool lost_on_open(const Entry&) { return false; }
#endif

} // namespace

std::expected<Walked, std::string> walk(const fs::path& root, std::stop_token stop) {
    std::error_code ec;
    const auto st = fs::status(root, ec); // the folder itself as picked: through a link
    if (ec || !fs::exists(st)) return util::failf("there is no folder {}", util::path_to_utf8(root));
    if (!fs::is_directory(st)) return util::failf("{} isn't a folder", util::path_to_utf8(root));

    Walked out;
    struct Pending {
        fs::path dir; // readable()
        std::string rel;
    };
    std::vector<Pending> todo{Pending{.dir = readable(root), .rel = {}}};
    bool first = true;
    while (!todo.empty()) {
        if (stop.stop_requested()) return Walked{};
        const Pending at = std::move(todo.back());
        todo.pop_back();
        auto listed = entries(at.dir);
        if (!listed) {
            if (first) return util::failf("cannot read {}: {}", util::path_to_utf8(root), listed.error());
            out.unreadable.push_back(std::format("{}: {}", at.rel, listed.error()));
            continue;
        }
        first = false;
        // grab's own partial downloads: records that parse, and the data files they go with.
        std::set<std::string> partial;
        for (const Entry& e : *listed) {
            if (e.link || e.dir || !e.utf8.ends_with(record_suffix)) continue;
            if (!is_record(at.dir / e.name)) continue;
            partial.insert(e.utf8);
            partial.insert(e.utf8.substr(0, e.utf8.size() - (record_suffix.size() - part_suffix.size())));
        }
        for (Entry& e : *listed) {
            const std::string rel = joined(at.rel, rclone_enc::from_raw_name(e.utf8));
            if (e.link) {
                ++out.links;
                continue;
            }
            if (!utf8_round_trips(e)) {
                out.unreadable.push_back(std::format("{}: its name can't be written in UTF-8", rel));
                continue;
            }
            if (lost_on_open(e)) { // nor gone into, for a folder: the listing would be another's
                out.unreadable.push_back(std::format("{}: its name ends with a dot or a space, which Windows programs can't open", rel));
                continue;
            }
            if (e.dir) {
                out.folders.push_back(rel);
                todo.push_back(Pending{.dir = at.dir / e.name, .rel = rel});
                continue;
            }
            if (partial.contains(e.utf8)) {
                ++out.partials;
                continue;
            }
            out.files.push_back(folder::FolderFile{.path = rel, .size = e.size, .modtime = rfc3339_ns(e.mtime_ns)});
        }
    }
    std::ranges::sort(out.files, {}, &folder::FolderFile::path);
    std::ranges::sort(out.folders);
    std::ranges::sort(out.unreadable);
    return out;
}

Left left_out(const fs::path& path) {
#ifdef _WIN32
    // Its own entry, as a folder's listing gives it: the reparse tag says a link or a junction.
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileExW(readable(path).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return Left::no;
    FindClose(h);
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 && IsReparseTagNameSurrogate(fd.dwReserved0)) return Left::link;
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return Left::no;
#else
    std::error_code ec;
    const auto st = fs::symlink_status(path, ec);
    if (ec) return Left::no;
    if (fs::is_symlink(st)) return Left::link;
    if (fs::is_directory(st)) return Left::no;
#endif
    const std::string name = util::path_to_utf8(path.filename());
    if (name.ends_with(record_suffix)) return is_record(path) ? Left::partial : Left::no;
    if (name.ends_with(part_suffix)) {
        fs::path record = path;
        record += util::path_from_utf8(record_suffix.substr(part_suffix.size()));
        return is_record(record) ? Left::partial : Left::no;
    }
    return Left::no;
}

fs::path local_path(const fs::path& root, std::string_view rel) {
    fs::path p = root;
    const std::string raw = rclone_enc::raw_path(rel);
    std::string_view rest = raw;
    while (!rest.empty()) {
        const auto slash = rest.find('/');
        p /= util::path_from_utf8(rest.substr(0, slash));
        if (slash == std::string_view::npos) break;
        rest.remove_prefix(slash + 1);
    }
    return p;
}

} // namespace grab::local_walk
