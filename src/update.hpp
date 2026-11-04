#pragma once

#include <array>
#include <compare>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace grab::update {

inline constexpr int exit_update_failed = 5;

inline constexpr std::string_view default_api_url =
    "https://api.github.com/repos/Giorgi-Apriashvili/grab/releases/latest";

// Registry key the Inno Setup installer writes (see AppId in installer/grab.iss.in).
inline constexpr std::string_view installer_uninstall_key =
    "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
    "{B7E9C1D4-5F2A-4C3E-9A8B-2D6F1E0C7A55}_is1";

inline constexpr std::string_view sums_asset_name = "SHA256SUMS.txt";

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
    auto operator<=>(const Version&) const = default;
};

// "1.2.3" or "v1.2.3"; anything else (pre-release suffixes included) is rejected.
[[nodiscard]] std::optional<Version> parse_version(std::string_view text);
[[nodiscard]] std::string to_string(const Version& v);

struct Asset {
    std::string name;
    std::string url;
};

struct Release {
    std::string tag;
    Version version;
    std::vector<Asset> assets;
    std::string page; // the release's web page (html_url), only when is_release_page()
};

// A GitHub `releases/latest` JSON payload.
[[nodiscard]] std::expected<Release, std::string> parse_release(std::string_view json_text);
// Whether `url` is a page on github.com, the only kind grab-gui opens for "What's new".
[[nodiscard]] bool is_release_page(std::string_view url);
[[nodiscard]] const Asset* find_asset(const Release& release, std::string_view name);

enum class InstallMode {
    installer, // installed by the Inno Setup installer: update by running the new installer
    portable   // zip or `cmake --install`: swap the programs in place
};

// grab-<v>-windows-x64-setup.exe or grab-<v>-windows-x64.zip
[[nodiscard]] std::string asset_name(const Version& v, InstallMode mode);

// Lowercase hex digest for `file_name` from sha256sum-style text ("<hex>  <name>" or
// "<hex> *<name>", LF or CRLF).
[[nodiscard]] std::optional<std::string> find_sha256(std::string_view sums_text,
                                                     std::string_view file_name);

// Windows directory equality: case-insensitive, '/' == '\', repeated and trailing separators
// ignored (Inno stores InstallLocation with a trailing '\', so "<loc>\bin" doubles it).
[[nodiscard]] bool same_dir(std::string_view a, std::string_view b);

// The programs a release ships next to grab.exe: grab.exe, and grab-gui.exe when this build has
// the window.
[[nodiscard]] std::vector<std::string> release_programs();

// Those of `programs` that are not files in `dir`, in the same order.
[[nodiscard]] std::vector<std::string> missing_programs(const std::filesystem::path& dir,
                                                        const std::vector<std::string>& programs);

// Whether `grab update` should install `latest`: it is newer, or it is this very version and the
// (portable) folder lacks some of its programs. A v0.2.0 updater extracted only grab.exe, so the
// folder it updated needs completing. Never a downgrade.
[[nodiscard]] bool needs_install(const Version& current, const Version& latest, bool folder_incomplete);

// For grab's startup: "note: ... missing next to grab.exe; run `grab update` to add them" when
// this is a portable copy lacking release programs, else nullopt. Windows only.
[[nodiscard]] std::optional<std::string> incomplete_folder_note();

// grab 0.20 and earlier shipped rclone 1.75.1 next to grab.exe, the only rclone a grab ever
// bundled; 0.20's portable update swaps only the programs of the new zip, so it stays behind. That
// copy is known by its exact size and SHA-256: any other rclone.exe is someone else's (G31).
inline constexpr std::uintmax_t old_rclone_size = 85'192'704;
inline constexpr std::string_view old_rclone_sha256 =
    "033eee51c9ad47c2de2624b6674d355274bcd6cf0027a5f85db4437ba24ae81c";
// Where remove_old_rclone puts that copy when it runs and so can't be deleted yet.
inline constexpr std::string_view old_rclone_aside = "rclone.exe.grab-old";

// The names grab gives files it replaced or put aside while they ran (update.cpp's swap,
// remove_old_rclone): only these are grab's to delete. Other programs in a shared folder keep their
// own .old files (G31), rclone.exe.old included: remove_old_rclone deletes that one only when it is
// grab's old rclone.
inline constexpr std::array<std::string_view, 3> replaced_programs{"grab.exe.old", "grab-gui.exe.old",
                                                                    old_rclone_aside};

// Deletes the replaced_programs files an update left in `dir`. Each goes once nothing runs it.
void remove_replaced_programs(const std::filesystem::path& dir);

// What remove_old_rclone did with one file.
enum class OldRclone {
    none,       // no plain file of that name (a folder or a link is never grab's copy)
    other_size, // someone else's: never opened
    not_opened, // grab's size, but it can't be opened to be deleted (a folder grab can't change, a
                // file being written): not hashed either
    other_file, // opened, but not grab's copy (another SHA-256, or unreadable): left as it was
    renamed,    // grab's copy, running: renamed to old_rclone_aside, which a later start deletes
    removed,    // grab's copy, deleted
    stuck       // grab's copy, neither renamed nor deleted
};
struct OldRclones {
    bool skipped = false;            // grab.exe.old or grab-gui.exe.old is there: nothing looked at
    OldRclone exe = OldRclone::none; // rclone.exe
    OldRclone old = OldRclone::none; // rclone.exe.old
};
// Deletes the rclone.exe and rclone.exe.old in `dir` that are a grab's old rclone: exactly `size`
// bytes, then that SHA-256, read through the one handle (read and delete access, nobody writing)
// that renames the file to old_rclone_aside and deletes it. Skipped while grab.exe.old or
// grab-gui.exe.old is still there: an old grab-gui may still run and start that rclone. Windows
// only; the size and SHA-256 are for the tests.
OldRclones remove_old_rclone(const std::filesystem::path& dir, std::uintmax_t size = old_rclone_size,
                             std::string_view sha256 = old_rclone_sha256);

// At the start of grab and grab-gui: remove_replaced_programs, then remove_old_rclone, next to the
// running program.
void clean_up_at_start();

// GRAB_UPDATE_API, else default_api_url.
[[nodiscard]] std::string api_url();

// What `grab update` and grab-gui know after asking for the latest release.
struct Check {
    Version current; // this build
    Release release;
    InstallMode mode = InstallMode::portable; // how the folder of the running program was installed
    std::vector<std::string> missing;         // programs a portable folder lacks
    // A newer release, or this one completing a portable folder (needs_install).
    [[nodiscard]] bool available() const { return needs_install(current, release.version, !missing.empty()); }
};
// Asks api_url() for the latest release (curl). `detached`: curl runs without a console window,
// for grab-gui, which can also `stop` it. Windows only.
[[nodiscard]] std::expected<Check, std::string> check(bool detached = false, std::stop_token stop = {});

// grab-gui checks at most once a day: whether a check last made at `last` (Unix seconds, 0 =
// never) is due at `now`. A clock that went backwards makes it due.
[[nodiscard]] bool update_due(std::int64_t last, std::int64_t now);

// The lines `grab update` prints at each step start with these; grab-gui follows the steps by
// them (phase_of), so they are defined once.
namespace text {
inline constexpr std::string_view checking = "checking for updates...";  // after "grab X.Y.Z: "
inline constexpr std::string_view downloading = "downloading ";
inline constexpr std::string_view verified = "verified SHA-256 ";
inline constexpr std::string_view installing = "running the installer silently...";
inline constexpr std::string_view updated = "updated grab ";
inline constexpr std::string_view completed = "completed grab ";
inline constexpr std::string_view up_to_date = " is up to date";         // "grab X.Y.Z is up to date ..."
inline constexpr std::string_view error = "grab: error: ";
} // namespace text

enum class Phase { other, checking, downloading, verified, installing, done, up_to_date, failed };
// Which step a line of `grab update` output reports.
[[nodiscard]] Phase phase_of(std::string_view line);

// `grab update [--check]`. Prints progress and returns the process exit code.
[[nodiscard]] int run(bool check_only);

} // namespace grab::update
