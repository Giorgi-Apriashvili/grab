#include "update.hpp"

#include "json.hpp"
#include "process.hpp"
#include "util.hpp"

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <format>
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

    if (const auto* assets = doc->find("assets"); assets && assets->kind == json::Value::Kind::array) {
        for (const auto& a : assets->items) {
            auto name = a.string_of("name");
            auto url = a.string_of("browser_download_url");
            if (name && url) r.assets.push_back(Asset{std::move(*name), std::move(*url)});
        }
    }
    return r;
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

bool same_dir(std::string_view a, std::string_view b) {
    auto norm = [](std::string_view s) {
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
    };
    return norm(a) == norm(b);
}

namespace {

// Keeps our buffered stdout lines ahead of stderr and of child output (curl, the installer)
// when the console output is redirected.
void flush() { std::fflush(stdout); }

void error(std::string_view msg) {
    flush();
    std::println(stderr, "grab: error: {}", msg);
}

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

#endif

} // namespace

int run(bool check_only) {
#ifndef _WIN32
    (void)check_only;
    error("`grab update` is only available on Windows; update a source build with git pull and "
          "rebuild");
    return 1;
#else
    const auto current = parse_version(GRAB_VERSION).value_or(Version{});
    const std::string api = util::getenv_utf8("GRAB_UPDATE_API").value_or(std::string(default_api_url));

    std::println("grab {}: checking for updates...", to_string(current));
    flush();
    auto meta = proc::run_capture(
        std::vector<std::string>{"curl", "-fsSL", "-H", "Accept: application/vnd.github+json", api});
    if (!meta) {
        error(std::format("{} (curl ships with Windows 10 1803 and later)", meta.error()));
        return exit_update_failed;
    }
    if (meta->exit_code != 0) {
        error(std::format("could not fetch {} (curl exit {})", api, meta->exit_code));
        return exit_update_failed;
    }
    auto release = parse_release(meta->out);
    if (!release) {
        error(release.error());
        return exit_update_failed;
    }

    const auto latest = release->version;
    if (latest <= current) {
        std::println("grab {} is up to date (latest release: {})", to_string(current), release->tag);
        return 0;
    }
    if (check_only) {
        std::println("grab {} is installed, {} is available; run `grab update` to install it",
                     to_string(current), to_string(latest));
        return 0;
    }

    // Decide how this copy was installed.
    const auto exe = util::self_exe_path();
    if (exe.empty()) {
        error("cannot determine the path of the running grab.exe");
        return exit_update_failed;
    }
    const auto location = installer_location();
    const bool via_installer =
        location && same_dir(*location + "\\bin", util::path_to_utf8(exe.parent_path()));
    const InstallMode mode = via_installer ? InstallMode::installer : InstallMode::portable;

    const std::string name = asset_name(latest, mode);
    const Asset* asset = find_asset(*release, name);
    const Asset* sums = find_asset(*release, sums_asset_name);
    if (asset == nullptr) {
        error(std::format("release {} has no {}", release->tag, name));
        return exit_update_failed;
    }
    if (sums == nullptr) {
        error(std::format("release {} has no {}; refusing to install an unverified download",
                          release->tag, sums_asset_name));
        return exit_update_failed;
    }

    std::error_code ec;
    const auto tmp =
        std::filesystem::temp_directory_path(ec) / std::format("grab-update-{}", to_string(latest));
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp, ec);
    if (ec) {
        error(std::format("cannot create {}: {}", util::path_to_utf8(tmp), ec.message()));
        return exit_update_failed;
    }
    struct Cleanup {
        std::filesystem::path dir;
        ~Cleanup() {
            std::error_code ignored;
            std::filesystem::remove_all(dir, ignored);
        }
    } cleanup{tmp};

    std::println("downloading {} ({} install)", name, via_installer ? "installer" : "portable");
    const auto sums_file = tmp / std::string(sums_asset_name);
    const auto payload = tmp / name;
    if (auto r = download(sums->url, sums_file, false); !r) {
        error(r.error());
        return exit_update_failed;
    }
    if (auto r = download(asset->url, payload, true); !r) {
        error(r.error());
        return exit_update_failed;
    }

    // Verify before touching anything.
    auto sums_text = util::read_file(sums_file);
    if (!sums_text) {
        error(sums_text.error());
        return exit_update_failed;
    }
    const auto expected = find_sha256(*sums_text, name);
    if (!expected) {
        error(std::format("{} has no entry for {}", sums_asset_name, name));
        return exit_update_failed;
    }
    auto actual = util::sha256_file(payload);
    if (!actual) {
        error(actual.error());
        return exit_update_failed;
    }
    if (*actual != *expected) {
        error(std::format("checksum mismatch for {}: expected {}, got {}; nothing was changed",
                          name, *expected, *actual));
        return exit_update_failed;
    }
    std::println("verified SHA-256 {}", *actual);

    if (mode == InstallMode::portable) {
        auto untar = proc::run_capture(std::vector<std::string>{
            "tar", "-xf", util::path_to_utf8(payload), "-C", util::path_to_utf8(tmp), "grab.exe"});
        const auto fresh = tmp / "grab.exe";
        if (!untar || untar->exit_code != 0 || !std::filesystem::exists(fresh, ec)) {
            error(std::format("cannot extract grab.exe from {}", name));
            return exit_update_failed;
        }
        auto old = move_self_aside(exe);
        if (!old) {
            error(old.error());
            return exit_update_failed;
        }
        std::filesystem::copy_file(fresh, exe, std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            restore_self(*old, exe);
            error(std::format("cannot write {}: {}; the previous version was restored",
                              util::path_to_utf8(exe), ec.message()));
            return exit_update_failed;
        }
        std::println("updated grab {} -> {} at {}", to_string(current), to_string(latest),
                     util::path_to_utf8(exe));
        std::println("(only grab.exe was replaced; the example config and README next to it are "
                     "unchanged)");
        return 0;
    }

    // Installer mode: free the exe path, then let the new installer upgrade in place. It reuses
    // the previous install directory, updates the Add/Remove Programs entry and keeps PATH.
    auto old = move_self_aside(exe);
    if (!old) {
        error(old.error());
        return exit_update_failed;
    }
    std::println("running the installer silently...");
    flush();
    auto code = proc::run_inherit(std::vector<std::string>{
        util::path_to_utf8(payload), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"});
    if (!code || *code != 0) {
        restore_self(*old, exe);
        error(code ? std::format("the installer failed (exit {}); the previous version was kept", *code)
                   : code.error());
        return exit_update_failed;
    }
    if (!std::filesystem::exists(exe, ec)) {
        restore_self(*old, exe);
        error("the installer finished but grab.exe is missing; the previous version was restored");
        return exit_update_failed;
    }
    std::println("updated grab {} -> {}", to_string(current), to_string(latest));
    return 0;
#endif
}

} // namespace grab::update
