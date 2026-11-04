#include "update.hpp"

#include "json.hpp"
#include "process.hpp"
#include "util.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <print>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)
#endif

#ifndef GRAB_VERSION
#define GRAB_VERSION "0.0.0"
#endif

namespace grab::update {

std::optional<Version> parse_version(std::string_view text) {
    text = util::trim(text);
    if (text.starts_with('v') || text.starts_with('V')) text.remove_prefix(1);
    const auto parts = util::split(text, '.');
    if (parts.size() != 3) return std::nullopt;
    int nums[3] = {};
    for (int i = 0; i < 3; ++i) {
        const auto& p = parts[static_cast<std::size_t>(i)];
        const auto [ptr, ec] = std::from_chars(p.data(), p.data() + p.size(), nums[i]);
        if (ec != std::errc{} || ptr != p.data() + p.size() || nums[i] < 0) return std::nullopt;
    }
    return Version{nums[0], nums[1], nums[2]};
}

std::string to_string(const Version& v) { return std::format("{}.{}.{}", v.major, v.minor, v.patch); }

std::expected<Release, std::string> parse_release(std::string_view json_text) {
    auto doc = json::parse(json_text);
    if (!doc) return util::failf("release metadata: {}", doc.error());
    if (doc->kind != json::Value::Kind::object) return util::fail("release metadata is not an object");

    Release r;
    auto tag = doc->string_of("tag_name");
    if (!tag) {
        // GitHub error payloads look like {"message": "Not Found", ...}.
        if (auto msg = doc->string_of("message")) {
            return util::failf("GitHub answered: {}", *msg);
        }
        return util::fail("release metadata has no tag_name");
    }
    r.tag = *tag;
    auto version = parse_version(r.tag);
    if (!version) return util::failf("latest release tag '{}' is not a version like v1.2.3", r.tag);
    r.version = *version;

    if (auto page = doc->string_of("html_url"); page && is_release_page(*page)) r.page = *page;
    if (const auto* assets = doc->find("assets"); assets && assets->kind == json::Value::Kind::array) {
        for (const auto& a : assets->items) {
            auto name = a.string_of("name");
            auto url = a.string_of("browser_download_url");
            if (name && url) r.assets.push_back(Asset{std::move(*name), std::move(*url)});
        }
    }
    return r;
}

bool is_release_page(std::string_view url) {
    constexpr std::string_view prefix = "https://github.com/";
    // Nothing that could leave the page or reach a shell: no quotes, spaces or control bytes.
    return url.starts_with(prefix) && url.size() > prefix.size() &&
           url.find_first_of(" \"'<>\\`") == std::string_view::npos &&
           std::ranges::none_of(url, [](char c) { return static_cast<unsigned char>(c) < 0x20; });
}

const Asset* find_asset(const Release& release, std::string_view name) {
    for (const auto& a : release.assets) {
        if (a.name == name) return &a;
    }
    return nullptr;
}

std::string asset_name(const Version& v, InstallMode mode) {
    return std::format("grab-{}-windows-x64{}", to_string(v),
                       mode == InstallMode::installer ? "-setup.exe" : ".zip");
}

std::optional<std::string> find_sha256(std::string_view sums_text, std::string_view file_name) {
    std::size_t start = 0;
    while (start < sums_text.size()) {
        auto end = sums_text.find('\n', start);
        if (end == std::string_view::npos) end = sums_text.size();
        const auto line = util::trim(sums_text.substr(start, end - start));
        start = end + 1;

        const auto space = line.find_first_of(" \t");
        if (space == std::string_view::npos) continue;
        const auto hash = line.substr(0, space);
        auto name = util::trim(line.substr(space));
        if (name.starts_with('*')) name.remove_prefix(1); // binary-mode marker
        if (name != file_name || hash.size() != 64) continue;
        const std::string lower = util::to_lower(hash);
        if (lower.find_first_not_of("0123456789abcdef") != std::string::npos) continue;
        return lower;
    }
    return std::nullopt;
}

namespace {

// A folder as same_dir compares it.
std::string normalized_dir(std::string_view s) {
    const std::string lower = util::to_lower(util::trim(s));
    std::string out;
    for (std::size_t i = 0; i < lower.size(); ++i) {
        const char c = lower[i] == '/' ? '\\' : lower[i];
        // Collapse repeated separators ("grab\\bin"), but keep a leading UNC "\\".
        if (c == '\\' && i > 1 && out.ends_with('\\')) continue;
        out += c;
    }
    while (out.size() > 1 && out.ends_with('\\')) out.pop_back();
    return out;
}

} // namespace

bool same_dir(std::string_view a, std::string_view b) { return normalized_dir(a) == normalized_dir(b); }

std::string folder_key(std::string_view dir) {
    std::string bytes; // UTF-16LE, as the installer's strings are
    for (const char16_t ch : util::path_from_utf8(normalized_dir(dir)).u16string()) {
        bytes += static_cast<char>(ch & 0xFF);
        bytes += static_cast<char>(ch >> 8);
    }
    return util::sha256_hex(bytes);
}

#ifndef GRAB_RELEASE_PROGRAMS
#define GRAB_RELEASE_PROGRAMS "grab.exe"
#endif

std::vector<std::string> release_programs() { return util::split(GRAB_RELEASE_PROGRAMS, ','); }

std::vector<std::string> missing_programs(const std::filesystem::path& dir, const std::vector<std::string>& programs) {
    std::vector<std::string> missing;
    std::error_code ec;
    for (const auto& p : programs) {
        if (!std::filesystem::is_regular_file(dir / util::path_from_utf8(p), ec)) missing.push_back(p);
    }
    return missing;
}

bool needs_install(const Version& current, const Version& latest, bool folder_incomplete) {
    return latest > current || (latest == current && folder_incomplete);
}

void remove_replaced_programs(const std::filesystem::path& dir) {
    std::error_code ignored;
    for (const auto name : replaced_programs) {
        const auto old = dir / util::path_from_utf8(name);
        // Only a file: a folder of that name isn't one grab left.
        if (std::filesystem::is_regular_file(old, ignored)) std::filesystem::remove(old, ignored);
    }
}

#ifdef _WIN32

namespace {

// Renames the file `h` holds (open with delete access) to `to`, replacing a file of that name.
bool rename_open(HANDLE h, const std::filesystem::path& to) {
    const std::wstring& name = to.native();
    // FILE_RENAME_INFO ends in the name; the zeroed buffer has room for its terminating NUL too.
    std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + (name.size() * sizeof(wchar_t)));
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->ReplaceIfExists = TRUE;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(name.size() * sizeof(wchar_t));
    std::memcpy(info->FileName, name.data(), info->FileNameLength);
    return SetFileInformationByHandle(h, FileRenameInfo, info, static_cast<DWORD>(buffer.size())) != 0;
}

// remove_old_rclone for one file, `name` in `dir`.
OldRclone remove_if_old_rclone(const std::filesystem::path& dir, std::string_view name, std::uintmax_t size,
                               std::string_view sha256) {
    const auto file = dir / util::path_from_utf8(name);
    const auto aside = dir / util::path_from_utf8(old_rclone_aside);
    constexpr DWORD not_plain = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT;
    const auto size_of = [](DWORD high, DWORD low) { return (std::uintmax_t{high} << 32) | low; };
    // The name's own entry, a link not followed: a link named rclone.exe may lead to someone's
    // rclone elsewhere.
    WIN32_FILE_ATTRIBUTE_DATA entry{};
    if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &entry) || (entry.dwFileAttributes & not_plain) != 0) {
        return OldRclone::none;
    }
    if (size_of(entry.nFileSizeHigh, entry.nFileSizeLow) != size) return OldRclone::other_size;

    // One handle for the hash, the rename and the delete. Without FILE_SHARE_WRITE nobody can
    // write the file while it is open, so what goes is what was hashed. A folder grab can't change
    // refuses delete access here, before a byte is read, so it costs no hash at each start.
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return OldRclone::not_opened;
    struct Close {
        HANDLE h;
        ~Close() { CloseHandle(h); }
    } close{h};
    // The file opened may not be the one looked at a moment ago.
    BY_HANDLE_FILE_INFORMATION opened{};
    if (!GetFileInformationByHandle(h, &opened)) return OldRclone::unread;
    if ((opened.dwFileAttributes & not_plain) != 0 || size_of(opened.nFileSizeHigh, opened.nFileSizeLow) != size) {
        return OldRclone::other_file;
    }
    const auto hash = util::sha256_handle(h);
    if (!hash) return OldRclone::unread;
    if (*hash != sha256) return OldRclone::other_file;

    // Renamed first: a running copy can't be deleted, but it can be renamed, so `rclone` no longer
    // finds it here and replaced_programs deletes it at a later start.
    const bool renamed = rename_open(h, aside);
    FILE_DISPOSITION_INFO gone{};
    gone.DeleteFile = TRUE; // done when the handle closes
    if (SetFileInformationByHandle(h, FileDispositionInfo, &gone, sizeof(gone)) != 0) return OldRclone::removed;
    return renamed ? OldRclone::renamed : OldRclone::stuck;
}

} // namespace

OldRclones remove_old_rclone(const std::filesystem::path& dir, std::uintmax_t size, std::string_view sha256) {
    OldRclones r;
    std::error_code ec;
    for (const auto* replaced : {"grab.exe.old", "grab-gui.exe.old"}) {
        if (std::filesystem::exists(std::filesystem::symlink_status(dir / replaced, ec))) {
            r.skipped = true;
            return r;
        }
    }
    r.exe = remove_if_old_rclone(dir, "rclone.exe", size, sha256);
    r.old = remove_if_old_rclone(dir, "rclone.exe.old", size, sha256);
    return r;
}

#else

OldRclones remove_old_rclone(const std::filesystem::path&, std::uintmax_t, std::string_view) { return {}; }

#endif

OldRclones remove_old_rclone_once(const std::filesystem::path& dir, const std::filesystem::path& markers,
                                  std::uintmax_t size, std::string_view sha256) {
    const auto mark = markers / folder_key(util::path_to_utf8(dir));
    std::error_code ec;
    if (std::filesystem::exists(mark, ec)) return OldRclones{.done_before = true};
    const auto r = remove_old_rclone(dir, size, sha256);
    // Nothing to come back for: each file never was grab's, or is gone (one that couldn't be read
    // may still be grab's).
    const auto settled = [](OldRclone o) {
        return o == OldRclone::none || o == OldRclone::other_size || o == OldRclone::other_file ||
               o == OldRclone::removed;
    };
    if (r.skipped || !settled(r.exe) || !settled(r.old)) return r;
    if (std::filesystem::exists(std::filesystem::symlink_status(dir / util::path_from_utf8(old_rclone_aside), ec))) {
        return r;
    }
    std::filesystem::create_directories(markers, ec);
    // When it can't be written (or grab and grab-gui both start now), the next start makes another
    // pass, which costs no hash where nothing is left.
    util::write_new_file(mark, "");
    return r;
}

std::filesystem::path old_rclone_markers() { return util::local_data_home() / "grab" / "old-rclone"; }

void clean_up_at_start() {
    const auto self = util::self_exe_path();
    if (self.empty()) return;
    remove_replaced_programs(self.parent_path());
    remove_old_rclone_once(self.parent_path(), old_rclone_markers());
}

std::string api_url() { return util::getenv_utf8("GRAB_UPDATE_API").value_or(std::string(default_api_url)); }

bool update_due(std::int64_t last, std::int64_t now) {
    constexpr std::int64_t day = 24 * 60 * 60;
    return last <= 0 || now < last || now - last >= day;
}

std::string result_json(const Result& r) {
    using json::Value;
    Value v = Value::make_object();
    v.set("folder", Value::make_string(r.folder));
    v.set("from", Value::make_string(r.from));
    v.set("to", Value::make_string(r.to));
    v.set("mode", Value::make_string(r.mode));
    v.set("ok", Value::make_bool(r.ok));
    v.set("exit", Value::make_number(r.exit_code));
    if (r.installer_exit) v.set("installerExit", Value::make_number(*r.installer_exit));
    v.set("error", Value::make_string(r.error));
    v.set("setupLog", Value::make_string(r.setup_log));
    v.set("time", Value::make_number(static_cast<double>(r.time)));
    return json::stringify(v) + "\n";
}

std::optional<Result> parse_result(std::string_view json_text) {
    const auto doc = json::parse(json_text);
    if (!doc || doc->kind != json::Value::Kind::object) return std::nullopt;
    const auto* ok = doc->find("ok");
    auto to = doc->string_of("to");
    if (ok == nullptr || ok->kind != json::Value::Kind::boolean || !to) return std::nullopt;
    auto number = [&](std::string_view key) -> std::optional<double> {
        const auto* n = doc->find(key);
        if (n == nullptr || n->kind != json::Value::Kind::number) return std::nullopt;
        return n->number;
    };
    // A number out of an int's range (a file edited by hand) counts as missing: the cast would be
    // undefined.
    auto as_int = [&](std::string_view key) -> std::optional<int> {
        const auto n = number(key);
        if (!n || *n < std::numeric_limits<int>::min() || *n > std::numeric_limits<int>::max()) return std::nullopt;
        return static_cast<int>(*n);
    };
    Result r;
    r.folder = doc->string_of("folder").value_or("");
    r.from = doc->string_of("from").value_or("");
    r.to = std::move(*to);
    r.mode = doc->string_of("mode").value_or("");
    r.ok = ok->boolean;
    r.exit_code = as_int("exit").value_or(0);
    r.installer_exit = as_int("installerExit");
    r.error = doc->string_of("error").value_or("");
    r.setup_log = doc->string_of("setupLog").value_or("");
    // Unix seconds, as gui_state.cpp takes a limit's `until`.
    if (const auto time = number("time"); time && *time > 0 && *time < 1e15) r.time = static_cast<std::int64_t>(*time);
    return r;
}

std::filesystem::path result_path() { return util::local_data_home() / "grab" / "last-update.json"; }

std::expected<void, std::string> save_result(const std::filesystem::path& json_path, const Result& r,
                                             std::string_view log) {
    if (auto saved = util::write_file_atomic(json_path.parent_path() / "last-update.log", log); !saved) {
        return saved;
    }
    return util::write_file_atomic(json_path, result_json(r));
}

std::optional<Result> pending_failure(const std::filesystem::path& json_path, const Version& current,
                                      const std::filesystem::path& dir) {
    const auto text = util::read_file_if_exists(json_path);
    if (!text || !*text) return std::nullopt;
    auto r = parse_result(**text);
    // Another copy's (a portable one beside the installed one): its Try again would update this one.
    if (!r || r->ok || r->folder != folder_key(util::path_to_utf8(dir))) return std::nullopt;
    const auto to = parse_version(r->to);
    // An update since (or a copy installed by hand) has what it tried to install.
    if (to && *to <= current) return std::nullopt;
    return r;
}

std::string failure_text(const Result& r) {
    const std::string when = r.time > 0 ? util::local_date(r.time) : std::string();
    return std::format("the update to {}{} failed: {}", r.to, when.empty() ? "" : " (" + when + ")",
                       r.error.empty() ? std::format("grab update exited with code {}", r.exit_code) : r.error);
}

#ifdef _WIN32

FolderLock::~FolderLock() {
    if (handle_ != nullptr) CloseHandle(handle_);
}

FolderLock& FolderLock::operator=(FolderLock&& other) noexcept {
    if (this != &other) {
        if (handle_ != nullptr) CloseHandle(handle_);
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

std::optional<FolderLock> lock_folder(const std::filesystem::path& dir) {
    const std::wstring name = L"Local\\grab.update." + util::to_wide(folder_key(util::path_to_utf8(dir)));
    HANDLE h = CreateMutexW(nullptr, FALSE, name.c_str());
    if (h == nullptr) return std::nullopt;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(h);
        return std::nullopt;
    }
    FolderLock lock;
    lock.handle_ = h;
    return lock;
}

#else

FolderLock::~FolderLock() = default;
FolderLock& FolderLock::operator=(FolderLock&& other) noexcept {
    handle_ = std::exchange(other.handle_, nullptr);
    return *this;
}
std::optional<FolderLock> lock_folder(const std::filesystem::path&) { return FolderLock{}; }

#endif

Phase phase_of(std::string_view line) {
    line = util::trim(line);
    if (line.starts_with(text::error)) return Phase::failed;
    if (line.ends_with(text::checking)) return Phase::checking;
    if (line.starts_with(text::downloading)) return Phase::downloading;
    if (line.starts_with(text::verified)) return Phase::verified;
    if (line.starts_with(text::installing)) return Phase::installing;
    if (line.starts_with(text::updated) || line.starts_with(text::completed)) return Phase::done;
    if (line.find(text::up_to_date) != std::string_view::npos) return Phase::up_to_date;
    return Phase::other;
}

namespace {

// Keeps our buffered stdout lines ahead of stderr and of child output (curl, the installer)
// when the console output is redirected.
void flush() { std::fflush(stdout); }

// One `grab update`: what it prints, kept for last-update.log, and what it did, for
// last-update.json.
struct Session {
    Result result;
    std::string log;
    bool attempted = false; // it set out to install a release: its result is kept
    // grab-gui was closed for the installer from this folder: started again at the end.
    std::optional<std::filesystem::path> gui_dir;
    // This run is elevated and that window wasn't: started again as it was, with its user's token
    // (a HANDLE, when it could be kept), never elevated (Windows).
    bool gui_unelevated = false;
    std::shared_ptr<void> gui_token;

    void say(std::string_view line) {
        std::println("{}", line);
        log.append(line).push_back('\n');
    }
    void error(std::string_view msg) {
        flush();
        std::println(stderr, "{}{}", text::error, msg);
        log.append(text::error).append(msg).push_back('\n');
        result.error = msg;
    }
};

#ifdef _WIN32

// Install directory recorded by the Inno Setup installer, if it installed grab for this user.
std::optional<std::string> installer_location() {
    const std::wstring key = util::to_wide(installer_uninstall_key);
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"InstallLocation", RRF_RT_REG_SZ, nullptr,
                     nullptr, &size) != ERROR_SUCCESS ||
        size == 0) {
        return std::nullopt;
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"InstallLocation", RRF_RT_REG_SZ, nullptr,
                     value.data(), &size) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    value.resize(wcsnlen(value.c_str(), value.size()));
    return util::to_utf8(value);
}

// curl -fL <url> -o <file>, quiet unless `progress`.
std::expected<void, std::string> download(const std::string& url, const std::filesystem::path& file,
                                          bool progress) {
    std::vector<std::string> argv{"curl", "-fL", progress ? "--progress-bar" : "-sS", "-o",
                                  util::path_to_utf8(file), url};
    flush();
    auto code = proc::run_inherit(argv);
    if (!code) return util::fail(code.error());
    if (*code != 0) return util::failf("download of {} failed (curl exit {})", url, *code);
    return {};
}

// Moves the running exe aside so its path can be written. Returns the backup path.
std::expected<std::filesystem::path, std::string> move_self_aside(const std::filesystem::path& exe) {
    auto old = exe;
    old += ".old";
    std::error_code ec;
    std::filesystem::remove(old, ec); // leftover from a previous update
    std::filesystem::rename(exe, old, ec);
    if (ec) {
        return util::failf("cannot move {} aside: {} (is another grab still running?)",
                           util::path_to_utf8(exe), ec.message());
    }
    return old;
}

void restore_self(const std::filesystem::path& old, const std::filesystem::path& exe) {
    std::error_code ec;
    if (!std::filesystem::exists(exe, ec)) std::filesystem::rename(old, exe, ec);
}

// True when `exe_dir` is the bin folder of the Inno Setup installation.
bool installed_by_installer(const std::filesystem::path& exe_dir) {
    const auto location = installer_location();
    return location && same_dir(*location + "\\bin", util::path_to_utf8(exe_dir));
}

// The grab-gui windows open from `dir`, with their processes (one each), whose handles close with
// it. Another copy's windows (a portable one, a build) aren't this update's.
struct Guis {
    std::vector<HWND> windows;
    std::vector<HANDLE> processes;
    // This process is elevated and a window isn't (`grab update` from an elevated console): that
    // window's token, duplicated as a primary one to start it again with (when it could be).
    bool unelevated = false;
    std::shared_ptr<void> token;
    Guis() = default;
    ~Guis() {
        for (HANDLE p : processes) CloseHandle(p);
    }
    Guis(const Guis&) = delete;
    Guis& operator=(const Guis&) = delete;
};

// Whether the token of `process` is elevated (false when it can't be told).
bool elevated(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION e{};
    DWORD size = 0;
    const bool yes = GetTokenInformation(token, TokenElevation, &e, sizeof(e), &size) != 0 && e.TokenIsElevated != 0;
    CloseHandle(token);
    return yes;
}

// The primary token of `process`, for CreateProcessWithTokenW; null when it can't be had.
std::shared_ptr<void> primary_token(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &token)) return nullptr;
    HANDLE primary = nullptr;
    const bool ok = DuplicateTokenEx(token, TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY, nullptr,
                                     SecurityImpersonation, TokenPrimary, &primary) != 0;
    CloseHandle(token);
    if (!ok) return nullptr;
    return {primary, CloseHandle};
}

void find_guis(const std::filesystem::path& dir, Guis& found) {
    const bool self_elevated = elevated(GetCurrentProcess());
    std::vector<DWORD> seen;
    for (HWND w = FindWindowExW(nullptr, nullptr, gui_window_class, nullptr); w != nullptr;
         w = FindWindowExW(nullptr, w, gui_window_class, nullptr)) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == 0 || std::ranges::find(seen, pid) != seen.end()) continue;
        seen.push_back(pid);
        HANDLE p = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (p == nullptr) continue;
        std::wstring image(32768, L'\0');
        auto size = static_cast<DWORD>(image.size());
        bool ours = false;
        if (QueryFullProcessImageNameW(p, 0, image.data(), &size) != 0) {
            image.resize(size);
            const auto from = std::filesystem::path(image).parent_path();
            // The same folder under another spelling too (a short 8.3 name, a link to it).
            std::error_code ec;
            ours = same_dir(util::path_to_utf8(from), util::path_to_utf8(dir)) ||
                   std::filesystem::equivalent(from, dir, ec);
        }
        if (ours) {
            found.windows.push_back(w);
            found.processes.push_back(p);
            if (self_elevated && !elevated(p)) {
                found.unelevated = true;
                if (!found.token) found.token = primary_token(p);
            }
        } else {
            CloseHandle(p);
        }
    }
}

// Asks each window to save and close (gui_close_request), waits for its process up to
// gui_exit_wait, and ends one still there. True when every one has ended.
bool close_guis(const Guis& guis) {
    const UINT request = RegisterWindowMessageW(gui_close_request);
    for (HWND w : guis.windows) PostMessageW(w, request, 0, 0);
    const auto until = std::chrono::steady_clock::now() + gui_exit_wait;
    bool all = true;
    for (HANDLE p : guis.processes) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
        if (WaitForSingleObject(p, static_cast<DWORD>(std::max<long long>(left.count(), 0))) == WAIT_OBJECT_0) continue;
        TerminateProcess(p, 1); // its queue was saved the last time it changed
        all = WaitForSingleObject(p, 5000) == WAIT_OBJECT_0 && all;
    }
    return all;
}

// grab-gui from `dir` with the user's own (unelevated) `token`: through the Secondary Logon service,
// so outside this process's job too, with this process's environment (an isolated %APPDATA% too).
std::expected<void, std::string> start_gui_unelevated(const std::filesystem::path& dir, HANDLE token) {
    const auto program = dir / "grab-gui.exe";
    std::wstring cmdline = L"\"" + program.native() + L"\"";
    const std::unique_ptr<wchar_t, decltype(&FreeEnvironmentStringsW)> env(GetEnvironmentStringsW(),
                                                                          &FreeEnvironmentStringsW);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessWithTokenW(token, 0, program.c_str(), cmdline.data(), CREATE_UNICODE_ENVIRONMENT, env.get(),
                                nullptr, &si, &pi) == 0) {
        return util::fail(util::error_message(GetLastError()));
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return {};
}

// grab-gui from `dir` again, on its own outside this process's job (as the window's own restart
// is): the new version after an installer update, the old one when it failed. Never elevated beyond
// the window that was closed: an elevated window takes no hand-over from a Start menu launch (UIPI
// drops it, and its lock can't be opened), so a second one would start on the same queue.
std::expected<void, std::string> start_gui(const std::filesystem::path& dir, bool unelevated, HANDLE token) {
    if (unelevated) {
        auto started = token != nullptr ? start_gui_unelevated(dir, token)
                                        : std::expected<void, std::string>(util::fail("its token couldn't be kept"));
        if (!started) {
            return util::failf("this update ran as administrator and grab-gui didn't, so it isn't started from here "
                               "({}); open grab from the Start menu",
                               started.error());
        }
        return {};
    }
    std::error_code ec;
    const auto output = std::filesystem::temp_directory_path(ec) / "grab-gui-restart.log";
    auto started =
        proc::start_independent(std::vector<std::string>{util::path_to_utf8(dir / "grab-gui.exe")}, output);
    if (!started) return util::fail(started.error());
    return {};
}

#endif

} // namespace

std::optional<std::string> incomplete_folder_note() {
#ifdef _WIN32
    const auto exe = util::self_exe_path();
    if (exe.empty() || installed_by_installer(exe.parent_path())) return std::nullopt;
    const auto missing = missing_programs(exe.parent_path(), release_programs());
    if (missing.empty()) return std::nullopt;
    return std::format("note: {} {} missing next to grab.exe; run `grab update` to add them",
                       util::join(missing, " and "), missing.size() == 1 ? "is" : "are");
#else
    return std::nullopt;
#endif
}

std::expected<Check, std::string> check(bool detached, std::stop_token stop) {
#ifndef _WIN32
    (void)detached;
    (void)stop;
    return util::fail("updates are only available on Windows");
#else
    Check c;
    c.current = parse_version(GRAB_VERSION).value_or(Version{});
    const std::string api = api_url();
    proc::RunOptions run;
    run.detached = detached;
    run.stop = std::move(stop);
    auto meta = proc::run_capture(std::vector<std::string>{"curl", "-fsSL", "--connect-timeout", "15", "--max-time", "60",
                                                           "-H", "Accept: application/vnd.github+json", api},
                                  run);
    if (run.stop.stop_requested()) return util::fail("stopped");
    if (!meta) return util::failf("{} (curl ships with Windows 10 1803 and later)", meta.error());
    if (meta->exit_code != 0) return util::failf("could not fetch {} (curl exit {})", api, meta->exit_code);
    auto release = parse_release(meta->out);
    if (!release) return util::fail(release.error());
    c.release = std::move(*release);

    // How this copy was installed. A portable folder may lack programs the release ships (a
    // v0.2.0 updater extracted only grab.exe); then the current release completes it.
    const auto exe = util::self_exe_path();
    if (exe.empty()) return util::fail("cannot determine the path of the running program");
    c.mode = installed_by_installer(exe.parent_path()) ? InstallMode::installer : InstallMode::portable;
    if (c.mode == InstallMode::portable) c.missing = missing_programs(exe.parent_path(), release_programs());
    return c;
#endif
}

#ifdef _WIN32

namespace {

// `grab update` itself; run() keeps its result and starts grab-gui again.
int update_once(bool check_only, Session& s) {
    const auto current = parse_version(GRAB_VERSION).value_or(Version{});
    s.say(std::format("grab {}: {}", to_string(current), text::checking));
    const auto exe = util::self_exe_path();
    // A failed update nobody saw: an installer update closes the window that started it.
    const auto failed = pending_failure(result_path(), current, exe.parent_path());
    if (failed) {
        s.say(std::format("note: {}; what it printed is in {}", failure_text(*failed),
                          util::path_to_utf8(result_path().parent_path() / "last-update.log")));
    }
    flush();
    // One at a time for these programs (review G96); --check changes nothing.
    std::optional<FolderLock> lock;
    if (!check_only) {
        lock = lock_folder(exe.parent_path());
        if (!lock) {
            s.error("another `grab update` is running for these programs; wait for it to end");
            return exit_update_failed;
        }
    }
    auto checked = check();
    if (!checked) {
        s.error(checked.error());
        return exit_update_failed;
    }
    const Release* release = &checked->release;
    const InstallMode mode = checked->mode;
    const bool via_installer = mode == InstallMode::installer;
    const auto& missing = checked->missing;

    const auto latest = release->version;
    if (!checked->available()) {
        s.say(std::format("grab {}{} (latest release: {})", to_string(current), text::up_to_date, release->tag));
        // The release that failed was withdrawn: nothing will rewrite its record, which the window
        // would show at every start.
        std::error_code ignored;
        if (failed && !check_only) std::filesystem::remove(result_path(), ignored);
        return 0;
    }
    const bool completing = latest == current;
    if (check_only) {
        if (completing) {
            s.say(std::format("grab {} is current, but {} lacks {}; run `grab update` to add them", to_string(current),
                              util::path_to_utf8(exe.parent_path()), util::join(missing, ", ")));
        } else {
            s.say(std::format("grab {} is installed, {} is available; run `grab update` to install it",
                              to_string(current), to_string(latest)));
        }
        return 0;
    }
    s.attempted = true;
    s.result.folder = folder_key(util::path_to_utf8(exe.parent_path()));
    s.result.from = to_string(current);
    s.result.to = to_string(latest);
    s.result.mode = via_installer ? "installer" : "portable";
    if (completing) {
        s.say(std::format("this folder lacks {}; adding them from {}", util::join(missing, ", "), release->tag));
    }

    const std::string name = asset_name(latest, mode);
    const Asset* asset = find_asset(*release, name);
    const Asset* sums = find_asset(*release, sums_asset_name);
    if (asset == nullptr) {
        s.error(std::format("release {} has no {}", release->tag, name));
        return exit_update_failed;
    }
    if (sums == nullptr) {
        s.error(std::format("release {} has no {}; refusing to install an unverified download", release->tag,
                            sums_asset_name));
        return exit_update_failed;
    }

    // A folder of this copy's own, so another copy's run can't delete its files (G96); runs of this
    // copy take turns (the lock), so this clears what a run that was killed left.
    std::error_code ec;
    const auto tmp = std::filesystem::temp_directory_path(ec) /
                     std::format("grab-update-{}", folder_key(util::path_to_utf8(exe.parent_path())).substr(0, 16));
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp, ec);
    if (ec) {
        s.error(std::format("cannot create {}: {}", util::path_to_utf8(tmp), ec.message()));
        return exit_update_failed;
    }
    struct Cleanup {
        std::filesystem::path dir;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(dir, ignored);
        }
    } cleanup{tmp};

    s.say(std::format("{}{} ({} install)", text::downloading, name, via_installer ? "installer" : "portable"));
    const auto sums_file = tmp / std::string(sums_asset_name);
    const auto payload = tmp / util::path_from_utf8(name); // the release's name, UTF-8 (G140)
    if (auto r = download(sums->url, sums_file, false); !r) {
        s.error(r.error());
        return exit_update_failed;
    }
    if (auto r = download(asset->url, payload, true); !r) {
        s.error(r.error());
        return exit_update_failed;
    }

    // Verify before touching anything.
    auto sums_text = util::read_file(sums_file);
    if (!sums_text) {
        s.error(sums_text.error());
        return exit_update_failed;
    }
    const auto expected = find_sha256(*sums_text, name);
    if (!expected) {
        s.error(std::format("{} has no entry for {}", sums_asset_name, name));
        return exit_update_failed;
    }
    auto actual = util::sha256_file(payload);
    if (!actual) {
        s.error(actual.error());
        return exit_update_failed;
    }
    if (*actual != *expected) {
        s.error(std::format("checksum mismatch for {}: expected {}, got {}; nothing was changed", name, *expected,
                            *actual));
        return exit_update_failed;
    }
    s.say(std::format("{}{}", text::verified, *actual));

    const auto dir = exe.parent_path();
    if (mode == InstallMode::portable) {
        const auto unpacked = tmp / "unpacked";
        std::filesystem::create_directories(unpacked, ec);
        auto untar = proc::run_capture(std::vector<std::string>{
            "tar", "-xf", util::path_to_utf8(payload), "-C", util::path_to_utf8(unpacked)});
        if (!untar || untar->exit_code != 0 || !std::filesystem::exists(unpacked / "grab.exe", ec)) {
            s.error(std::format("cannot extract {}", name));
            return exit_update_failed;
        }
        // Every program in the release (grab, grab-gui) is replaced together, so they stay a
        // tested set. Running ones cannot be overwritten, but they can
        // be renamed: each is moved aside as <name>.old, deleted on a later run.
        struct Swap {
            std::filesystem::path target;
            std::optional<std::filesystem::path> old; // nullopt: newly added file
        };
        std::vector<Swap> done;
        auto rollback = [&] {
            std::error_code rc;
            for (auto it = done.rbegin(); it != done.rend(); ++it) {
                std::filesystem::remove(it->target, rc);
                if (it->old) std::filesystem::rename(*it->old, it->target, rc);
            }
        };
        std::vector<std::string> replaced;
        for (const auto& entry : std::filesystem::directory_iterator(unpacked, ec)) {
            if (!entry.is_regular_file(ec) || entry.path().extension() != ".exe") continue;
            const auto target = dir / entry.path().filename();
            Swap swap{target, std::nullopt};
            if (std::filesystem::exists(target, ec)) {
                auto old = move_self_aside(target);
                if (!old) {
                    rollback();
                    s.error(old.error() + "; nothing was changed");
                    return exit_update_failed;
                }
                swap.old = *old;
            }
            done.push_back(swap);
            std::filesystem::copy_file(entry.path(), target, std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                const std::string why = ec.message();
                rollback();
                s.error(std::format("cannot write {}: {}; the previous version was restored", util::path_to_utf8(target),
                                    why));
                return exit_update_failed;
            }
            replaced.push_back(util::path_to_utf8(entry.path().filename()));
        }
        // Third-party notices travel with the programs they cover.
        if (std::filesystem::is_directory(unpacked / "licenses", ec)) {
            std::filesystem::create_directories(dir / "licenses", ec);
            std::filesystem::copy(unpacked / "licenses", dir / "licenses",
                                  std::filesystem::copy_options::overwrite_existing |
                                      std::filesystem::copy_options::recursive,
                                  ec);
        }
        if (completing) {
            s.say(std::format("{}{} in {} ({})", text::completed, to_string(latest), util::path_to_utf8(dir),
                              util::join(replaced, ", ")));
        } else {
            s.say(std::format("{}{} -> {} in {} ({})", text::updated, to_string(current), to_string(latest),
                              util::path_to_utf8(dir), util::join(replaced, ", ")));
        }
        // A running grab-gui keeps running the old program (it was renamed aside) until it restarts.
        Guis open;
        find_guis(dir, open);
        if (!open.processes.empty()) s.say("grab-gui is open: restart it to use the new version");
        return 0;
    }

    // Installer mode: free the exe path, then let the new installer upgrade in place. It reuses
    // the previous install directory, updates the Add/Remove Programs entry and keeps PATH.
    auto old = move_self_aside(exe);
    if (!old) {
        s.error(old.error());
        return exit_update_failed;
    }
    // grab-gui is closed here, not by the installer's Restart Manager, which closed it first and
    // then, when anything else held it up, gave up: the update was cancelled and nothing started
    // the window again (RCLONE-REPLACEMENT.md, Phase 9 chunk E). Asked, it saves its downloads
    // (running ones go on in the next one) and ends within gui_exit_deadline; run() starts it again
    // whatever the installer does.
    Guis open;
    find_guis(dir, open);
    if (!open.processes.empty()) {
        s.say("grab-gui is open: closing it for the installer; it opens again afterwards");
        flush();
        if (!close_guis(open)) s.say("note: grab-gui didn't end; the installer can't replace it while it runs");
        s.gui_dir = dir;
        s.gui_unelevated = open.unelevated;
        s.gui_token = open.token;
    }
    s.say(text::installing);
    flush();
    // Its log stays when it fails, named in the error.
    const auto setup_log = std::filesystem::temp_directory_path(ec) / std::format("grab-setup-{}.log", to_string(latest));
    // What it starts may outlive this process's job: a grab-gui it found open and closed itself (one
    // that didn't close above) it starts again when the install fails.
    auto code = proc::run_inherit(
        std::vector<std::string>{util::path_to_utf8(payload), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
                                 "/NOCLOSEAPPLICATIONS", "/NORESTARTAPPLICATIONS", "/LOG=" + util::path_to_utf8(setup_log)},
        false);
    if (code) s.result.installer_exit = *code;
    if (!code || *code != 0) {
        restore_self(*old, exe);
        if (code) s.result.setup_log = util::path_to_utf8(setup_log);
        s.error(code ? std::format("the installer failed (exit {}); the previous version was kept (setup log: {})", *code,
                                   s.result.setup_log)
                     : code.error());
        return exit_update_failed;
    }
    if (!std::filesystem::exists(exe, ec)) {
        restore_self(*old, exe);
        s.result.setup_log = util::path_to_utf8(setup_log);
        s.error(std::format("the installer finished but grab.exe is missing; the previous version was restored "
                            "(setup log: {})",
                            s.result.setup_log));
        return exit_update_failed;
    }
    std::filesystem::remove(setup_log, ec);
    s.say(std::format("{}{} -> {}", text::updated, to_string(current), to_string(latest)));
    return 0;
}

// Keeps what the run did for grab-gui's next start and the next `grab update`.
void keep(const Session& s) {
    if (auto saved = save_result(result_path(), s.result, s.log); !saved) {
        std::println(stderr, "grab: note: the update's result couldn't be kept: {}", saved.error());
    }
}

} // namespace

#endif

int run(bool check_only) {
#ifndef _WIN32
    (void)check_only;
    std::println(stderr, "{}`grab update` is only available on Windows; update a source build with git pull and rebuild",
                 text::error);
    return 1;
#else
    Session s;
    const int code = update_once(check_only, s);
    if (s.attempted) {
        s.result.ok = code == 0;
        s.result.exit_code = code;
        s.result.time = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
        keep(s); // before grab-gui starts again: it reads this at its start
    }
    // The window closed for the installer comes back, the new version or, when the update failed,
    // the old one, which then shows why.
    if (s.gui_dir) {
        if (auto started = start_gui(*s.gui_dir, s.gui_unelevated, s.gui_token.get()); !started) {
            s.error("grab-gui couldn't be started again: " + started.error());
            if (s.attempted) keep(s);
        }
    }
    return code;
#endif
}

} // namespace grab::update
