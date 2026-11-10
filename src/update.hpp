#pragma once

#include <array>
#include <chrono>
#include <compare>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
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

// Where grab is downloaded by hand: named when `grab update` can't, unless the release has its own
// page.
inline constexpr std::string_view releases_page = "https://github.com/Giorgi-Apriashvili/grab/releases";

struct Asset {
    std::string name;
    std::string url; // browser_download_url
    std::string api; // its API URL ("url"), only when is_api_asset_url(): the fallback download
};

struct Release {
    std::string tag;
    Version version;
    std::vector<Asset> assets;
    std::string page; // the release's web page (html_url), only when is_release_page()
};

// A GitHub `releases/latest` JSON payload, fetched from `api` (an asset's API URL is kept only when
// it is of that origin).
[[nodiscard]] std::expected<Release, std::string> parse_release(std::string_view json_text,
                                                                std::string_view api = default_api_url);
// Whether `url` is a page on github.com, the only kind grab-gui opens for "What's new".
[[nodiscard]] bool is_release_page(std::string_view url);
// Whether `url` (an asset's "url" in the release metadata) is an API asset URL at the origin
// (scheme, host and port) of `api`, the metadata's own URL, so a GRAB_UPDATE_API test server's
// count too: <origin>/repos/<owner>/<repo>/releases/assets/<digits>, nothing else (no query, no
// quotes, spaces or control bytes).
[[nodiscard]] bool is_api_asset_url(std::string_view url, std::string_view api);
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
// A name for the folder `dir` (UTF-8), the same for every spelling same_dir takes as that folder:
// the SHA-256 (lowercase hex) of the folder as same_dir compares it (ASCII lower case, '\', no
// trailing separator) in UTF-16LE. The installer computes it too (GetSHA256OfUnicodeString of
// LowerCase('{app}\bin')), so the two must stay alike.
[[nodiscard]] std::string folder_key(std::string_view dir);

// grab-gui's window class (src/gui/main_gui.cpp), by which `grab update` and the installer find
// the windows open.
inline constexpr const wchar_t* gui_window_class = L"grab.gui.window";
// A registered window message: `grab update` asks grab-gui to save and close, to replace it and
// start it again afterwards (running and queued downloads go on there).
inline constexpr const wchar_t* gui_close_request = L"grab.gui.close-for-update";
// How long grab-gui takes at most to end once it closes: its queue and settings are saved first,
// and after this it ends whatever still runs (the Restart Manager gives up on a program that takes
// more than about 30 s, and the update with it).
inline constexpr std::chrono::seconds gui_exit_deadline{5};
// How long `grab update`, and a grab-gui restarted after a portable update, wait for an old
// grab-gui to end before they end it: its deadline and room to spare (0.20.1's window has none,
// and took up to a minute with a login stuck).
inline constexpr std::chrono::seconds gui_exit_wait{15};

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
    other_file, // opened, but not grab's copy (another size or SHA-256): left as it was
    unread,     // grab's size, opened, but its information or bytes couldn't be read (a lock, an I/O
                // error, a placeholder offline): tried again at the next start
    renamed,    // grab's copy, running: renamed to old_rclone_aside, which a later start deletes
    removed,    // grab's copy, deleted
    stuck       // grab's copy, neither renamed nor deleted
};
struct OldRclones {
    bool skipped = false;            // grab.exe.old or grab-gui.exe.old is there: nothing looked at
    bool done_before = false;        // remove_old_rclone_once: an earlier start did it; nothing looked at
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

// The user's choice (2026-11-04): grab's old rclone goes once, right after the update to 0.21, and
// an rclone.exe put in the folder later is never touched, even one that is the same 1.75.1 build.
// remove_old_rclone_once runs remove_old_rclone in `dir` until a pass leaves nothing to come back
// for, then marks the folder done: an empty file named folder_key(dir) in `markers`. Not done
// while it was skipped, while grab's copy is still there (a folder grab can't change, a file it
// couldn't delete) or while its rclone.exe.grab-old is (it still ran: remove_replaced_programs
// deletes it at a later start). Once marked, nothing in the folder is looked at again. A folder
// never marked before is looked at too, so the first start of 0.21 in a folder that already held an
// rclone.exe of exactly that build removes it (docs/doc.md says to keep one's own rclone elsewhere).
OldRclones remove_old_rclone_once(const std::filesystem::path& dir, const std::filesystem::path& markers,
                                  std::uintmax_t size = old_rclone_size, std::string_view sha256 = old_rclone_sha256);
// Where clean_up_at_start keeps those marks: %LOCALAPPDATA%\grab\old-rclone. The installer deletes
// its folder's mark when it upgrades a grab older than 0.21, so going back to 0.20.1 (which brings
// its rclone.exe back) and updating again removes it once more.
[[nodiscard]] std::filesystem::path old_rclone_markers();

// At the start of grab and grab-gui: remove_replaced_programs, then remove_old_rclone_once, next to
// the running program.
void clean_up_at_start();

// What the last `grab update` that set out to install did, kept in %LOCALAPPDATA%\grab as
// last-update.json, with what it printed as last-update.log beside it. An installer update closes
// the window that started it, so nobody saw a failure: grab-gui shows it when it next opens, until
// it is dismissed, and `grab update` says it too (RCLONE-REPLACEMENT.md, Phase 9 chunk E).
struct Result {
    std::string folder; // folder_key of the programs' folder: the record is that copy's
    std::string from; // the version that ran it
    std::string to;   // the release it installed, or tried to
    std::string mode; // "installer" or "portable"
    bool ok = false;
    int exit_code = 0;                 // grab update's
    std::optional<int> installer_exit; // the installer's, when it ran
    std::string error;                 // its last error line, without text::error
    std::string setup_log;             // the installer's log, kept when it failed
    std::int64_t time = 0;             // when it ended, Unix seconds
};
[[nodiscard]] std::string result_json(const Result& r);
// nullopt for anything that isn't such a record.
[[nodiscard]] std::optional<Result> parse_result(std::string_view json_text);
[[nodiscard]] std::filesystem::path result_path(); // %LOCALAPPDATA%\grab\last-update.json
// Writes `r` to `json_path` and `log` beside it as last-update.log, each atomically.
[[nodiscard]] std::expected<void, std::string> save_result(const std::filesystem::path& json_path, const Result& r,
                                                           std::string_view log);
// The failed update kept at `json_path` while it still matters: it was this copy's (the programs in
// `dir`; the record is per user, and another copy may share it) and this build (`current`) is older
// than the release it tried to install. nullopt otherwise (none, a success, another copy's,
// unreadable).
[[nodiscard]] std::optional<Result> pending_failure(const std::filesystem::path& json_path, const Version& current,
                                                    const std::filesystem::path& dir);
// "the update to 0.21.1 (2026-11-04) failed: <its error>", for the command; the window shows the
// error itself.
[[nodiscard]] std::string failure_text(const Result& r);

// One `grab update` at a time for the programs in a folder (review G96): a named mutex keyed by
// folder_key, held while the object lives.
class FolderLock {
public:
    FolderLock() = default;
    ~FolderLock();
    FolderLock(FolderLock&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    FolderLock& operator=(FolderLock&& other) noexcept;
    FolderLock(const FolderLock&) = delete;
    FolderLock& operator=(const FolderLock&) = delete;

private:
    friend std::optional<FolderLock> lock_folder(const std::filesystem::path& dir);
    void* handle_ = nullptr;
};
// nullopt while another `grab update` holds it (in any process). Windows; elsewhere always taken.
[[nodiscard]] std::optional<FolderLock> lock_folder(const std::filesystem::path& dir);

// GRAB_UPDATE_API, else default_api_url.
[[nodiscard]] std::string api_url();

// ---- requests that survive a GitHub hiccup -------------------------------------------------------
// On 2026-11-08 GitHub's edge answered 504 for releases/download URLs and kept that answer for each
// URL for minutes, so every Try again failed at once, while the same file through the API came in
// half a second (grab-plans TODO.md, Updates). So each request is tried a few times, every try
// after the first at a URL no cache has seen (fresh_url), and a release file then through its API
// URL. The SHA-256 check stays the only trust in what comes.

// How one curl run ended.
struct Attempt {
    int curl_exit = 0;    // curl's exit code: 0 when an answer came, whatever its status
    int status = 0;       // the last answer's HTTP status (after redirects); 0: none came, or a file:// URL
    bool limited = false; // GitHub's rate limit refused it: never tried again
    std::string error;    // curl couldn't run at all: why (nothing more is tried)
    std::string url;      // the URL tried (fetch sets it)
    std::string host;     // where curl ended up, after redirects, when it said; else url's host
};
// Whether it brought the file: an answer with a 2xx status (or a file:// URL's, which has none).
[[nodiscard]] bool succeeded(const Attempt& a);
// Whether another try may heal it: a network failure (no name, no connection, a failed TLS
// handshake, a transfer cut short or stalled, an empty reply) or an answer 5xx, 408 or 429. Not
// 403, 404, 410 and the like, nor GitHub's rate limit, nor a curl stopped (proc::exit_stopped).
[[nodiscard]] bool worth_retrying(const Attempt& a);
// Whether an answer is GitHub's limit for requests without an account (60 an hour from one
// address): a 403 or 429 whose body says "rate limit". Another try now won't help.
[[nodiscard]] bool rate_limited(int curl_exit, int status, std::string_view body);
// What went wrong, in plain words: "GitHub answered 504 (Gateway Time-out)", "could not reach
// github.com (timed out)".
[[nodiscard]] std::string describe(const Attempt& a);
// `url` with "grab=<stamp>-<n>" in its query: a URL no cache has an answer for, unique to the run
// (`stamp`, Unix milliseconds) and to the try (`n`).
[[nodiscard]] std::string fresh_url(std::string_view url, std::int64_t stamp, int n);

// One file to fetch, and how often.
struct Fetch {
    std::string what;       // in the lines: "SHA256SUMS.txt", "the latest release"
    std::string url;        // tried first, the first time as it is
    std::string api;        // then this, the file's API URL (empty: no such fallback)
    std::int64_t stamp = 0; // the run's, in each fresh_url
    std::vector<std::chrono::seconds> waits;     // before each try of `url`: so also how many
    std::vector<std::chrono::seconds> api_waits; // before each try of `api`
};
// One try: curl for `url`; `api` true for the API URL, asked for the file's bytes.
using TryFn = std::function<Attempt(const std::string& url, bool api)>;
// Waits that long; false when it was stopped meanwhile.
using WaitFn = std::function<bool(std::chrono::seconds)>;
using SayFn = std::function<void(std::string_view)>;
// Tries `f.url`, then `f.api`, as their waits say: the very first try at the URL as it is (often a
// good answer cached), every later one at a fresh_url. A failure another try can't heal
// (worth_retrying) ends that URL's tries; curl not running, or stopped, ends them all. A failure
// with a try after it is said (`say`, may be empty) with what comes next ("GitHub answered 504
// (Gateway Time-out) for SHA256SUMS.txt; trying again in 3 s (try 2 of 6)"), and so is a success
// after failures. Returns each try's outcome in order; the last is the success when there is one.
[[nodiscard]] std::vector<Attempt> fetch(const Fetch& f, const TryFn& attempt, const WaitFn& wait, const SayFn& say);
// The error after tries that brought nothing (fetch): `lead`, what went wrong at each and what to
// do: "could not download SHA256SUMS.txt: GitHub answered 504 (Gateway Time-out) to every try, a
// problem on GitHub's side; try again later, or <by_hand>".
[[nodiscard]] std::string fetch_error(std::string_view lead, const std::vector<Attempt>& tries,
                                      std::string_view by_hand);

// What `grab update` and grab-gui know after asking for the latest release.
struct Check {
    Version current; // this build
    Release release;
    InstallMode mode = InstallMode::portable; // how the folder of the running program was installed
    std::vector<std::string> missing;         // programs a portable folder lacks
    // A newer release, or this one completing a portable folder (needs_install).
    [[nodiscard]] bool available() const { return needs_install(current, release.version, !missing.empty()); }
};
// Asks api_url() for the latest release (curl), trying again as fetch() does after a network
// failure or an answer 5xx, 408 or 429, with each failed try said through `say`. `detached`: curl
// runs without a console window, for grab-gui's check, which tries fewer times and can `stop` it.
// Windows only.
[[nodiscard]] std::expected<Check, std::string> check(bool detached = false, std::stop_token stop = {},
                                                      const SayFn& say = {});

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
// "<what went wrong> for <file>; trying again in 3 s (try 2 of 6)" (fetch)
inline constexpr std::string_view retrying = "; trying ";
// "got <file> on try 3 of 6", after failed tries (fetch)
inline constexpr std::string_view retried = "got ";
} // namespace text

enum class Phase { other, checking, downloading, verified, installing, done, up_to_date, failed, retrying, retried };
// Which step a line of `grab update` output reports.
[[nodiscard]] Phase phase_of(std::string_view line);

// `grab update [--check]`. Prints progress and returns the process exit code.
[[nodiscard]] int run(bool check_only);

} // namespace grab::update
