#pragma once

// The steps behind both front ends (CLI and GUI): load a remote's context, search it, and
// download what was picked. Nothing here reads stdin or writes to the console.

#include "cli.hpp"
#include "config.hpp"
#include "process.hpp"
#include "remote.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace grab::engine {

// Everything needed to talk to one remote.
struct Context {
    GrabConfig config;    // rclone and rclone_config resolved (bundled exe, grab's own file)
    RemoteSettings settings;
    RcloneRemote remote;  // connection details from rclone.conf
    FindMethod method;    // resolved: ssh or rclone (engine = rclone)
    std::vector<std::string> imported; // remotes imported into grab's rclone.conf just now
    Engine engine = Engine::rclone;    // effective_engine(settings)
    GrabMethod grab_method = GrabMethod::probe; // how searches and lists run on engine = grab
};

// grab.conf + the selected remote section + its rclone.conf entry. Errors are ready to show.
// Also performs the one-time import of referenced remotes into grab's own rclone.conf.
[[nodiscard]] std::expected<Context, std::string>
load_context(const std::filesystem::path& config_path, const std::optional<std::string>& remote_name);

// How grab's own SSH client logs in to the context's server (engine = grab): its rclone.conf
// section, with the secrets of grab's encrypted store, and any the rclone.conf in use keeps
// itself (obscured, as rclone writes them when it isn't grab's own file).
[[nodiscard]] std::expected<ssh::Target, std::string> ssh_target(const Context& ctx);

// rclone's version from `rclone version`'s output ("rclone v1.75.1").
struct RcloneVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;
    auto operator<=>(const RcloneVersion&) const = default;
};
[[nodiscard]] std::optional<RcloneVersion> parse_rclone_version(std::string_view out);
// The oldest rclone grab works with: --sftp-connections (every folder batch, verify) came in 1.67.
inline constexpr RcloneVersion min_rclone{1, 67, 0};
// Whether `rclone` can be used: the one grab ships always; another (grab.conf's `rclone =`
// when `configured`, else PATH's because the bundled one is missing) is asked its version once
// per process and refused below min_rclone (review G190).
[[nodiscard]] std::expected<void, std::string> check_rclone(const std::string& rclone,
                                                            const std::filesystem::path& exe_dir, bool configured);

// A search, list or Browse through grab's own SSH client (engine = grab): find over an exec
// channel where the server's check says it works, else the SFTP walk (RCLONE-REPLACEMENT.md,
// phase 4).
struct GrabSide {
    // The server's pool, shared with the program's other users of it (the window's downloads; a
    // command's download after its search, so it logs in once). None: one made for the call.
    std::shared_ptr<ssh::Pool> pool;
    // How many connections a walk takes, asked once the walk is chosen (the window takes its
    // fetch::Listing seat of up to 8 then). Unset: 4.
    std::function<int()> walk_seat;
    // How it goes (find or the walk), once chosen and before it starts.
    std::function<void(const ssh::Lookup&)> on_lookup;
    // Entries so far, now and then: found by find, looked at by the walk (from the walk's threads).
    std::function<void(std::size_t)> progress;
};

struct SearchRequest {
    std::string target; // words, glob, or a remote path containing '/'
    Mode mode = Mode::file;
    bool exact = false;
    std::optional<int> depth; // overrides the remote's max_depth
    bool batch_ssh = false;   // never let ssh prompt (GUI): -o BatchMode=yes
    // For a walk with rclone: its connections from the pool (fetch::Listing::flags).
    std::vector<std::string> walk_flags;
    GrabSide grab; // engine = grab
};

struct Hit {
    std::string path;                  // remote path, as given to rclone
    std::string name;                  // last path component
    std::optional<std::uint64_t> size; // files only
    bool dir = false;                  // a folder, where one search returns both kinds (browse)
    std::optional<std::int64_t> modified{}; // Unix seconds
};

struct SearchResult {
    std::vector<Hit> hits; // ranked best first (see rank_matches)
    std::vector<std::string> roots; // searched roots ("" = login home), for messages
    int max_depth = 0;
    // What the lookup couldn't look at while the rest was found (review G55): folders it couldn't
    // read, a search root that failed, find's loop warnings ("a/b: permission denied").
    std::vector<std::string> notes;
};

// Called with each external command just before it runs (for --verbose).
using CommandHook = std::function<void(const std::vector<std::string>&)>;

// The remote's search roots; "" (the login home) when none are set.
[[nodiscard]] std::vector<std::string> search_roots(const Context& ctx);

// Runs `command` on the server through ssh (never prompting with `batch_ssh`): its stdout, or a
// readable error when ssh itself failed (exit 255). Other exit codes count as success: find
// reports unreadable folders that way, and what it printed is still good.
[[nodiscard]] std::expected<std::string, std::string>
run_over_ssh(const Context& ctx, const std::string& command, bool batch_ssh, const proc::RunOptions& run = {},
             const CommandHook& on_command = {});

// Runs an `rclone lsf` command line: its stdout, or a readable error.
[[nodiscard]] std::expected<std::string, std::string>
run_listing(std::span<const std::string> argv, const proc::RunOptions& run = {}, const CommandHook& on_command = {});

// A search's result from the entries found: duplicates dropped, ranked best first
// (rank_matches), sizes and dates kept.
[[nodiscard]] SearchResult make_result(const Context& ctx, const SearchRequest& req, std::vector<RemoteEntry> entries);

// Runs the lookup the context's method calls for. An error means the lookup itself failed
// (ssh or rclone exited non-zero); no matches is a successful, empty result. On engine = grab:
// a path target is one SFTP STAT; a name search runs find or the walk (req.grab), and only
// `run.stop` of `run` counts.
[[nodiscard]] std::expected<SearchResult, std::string>
search(const Context& ctx, const SearchRequest& req, const proc::RunOptions& run = {},
       const CommandHook& on_command = {});
// Folders and files at once (the window's searches), `req.mode` aside: over rclone one walk lists
// both kinds, where a search per kind walked the server twice (review G197); over ssh, a find
// per kind; on engine = grab one find or one walk.
[[nodiscard]] std::expected<std::pair<SearchResult, SearchResult>, std::string>
search_both(const Context& ctx, const SearchRequest& req, const proc::RunOptions& run = {},
            const CommandHook& on_command = {});

// engine = grab: the pool a lookup uses, `side`'s, else one made for it (of 4 connections).
[[nodiscard]] std::expected<std::shared_ptr<ssh::Pool>, std::string> grab_pool(const Context& ctx,
                                                                              const GrabSide& side);
// How a search or list goes there: ssh::choose_lookup with the context's method (the check asked
// once per pool), told to side.on_lookup. "search cancelled" when stopped.
[[nodiscard]] std::expected<ssh::Lookup, std::string> grab_lookup(const Context& ctx, ssh::Pool& pool,
                                                                  const GrabSide& side, std::stop_token stop);
// The way chosen in words, after "searching" or "listing" (-v, the window): "with find (GNU
// find)", "by listing folders (a Storage Box has no find)", "by listing folders (its answer to
// grab's find check: ...)".
[[nodiscard]] std::string lookup_text(const Context& ctx, const ssh::Lookup& way);

// rclone argv for downloading `remote_path` into `dest_dir` (DEST\<name>), with the remote's
// flags as configured (including -P for the console). Fails as build_rclone_argv does.
[[nodiscard]] std::expected<std::vector<std::string>, std::string> download_argv(const Context& ctx, Mode mode,
                                                     const std::string& remote_path,
                                                     const std::filesystem::path& dest_dir,
                                                     std::span<const std::string> extra = {},
                                                     std::optional<std::filesystem::path> local_name = {});

// One file rclone is transferring right now.
struct Transfer {
    std::string name; // relative to the source folder
    std::uint64_t bytes = 0;
    std::uint64_t size = 0;
    double speed = 0;
};

// Live transfer state, parsed from rclone's --use-json-log stats lines.
struct Progress {
    std::uint64_t bytes = 0;
    std::uint64_t total = 0;
    double speed = 0;          // bytes per second
    std::optional<double> eta; // seconds
    std::string current;       // name of a file being transferred, if any
    std::vector<Transfer> transferring;
};

// A stats line ({"stats":{...}}) as Progress; nullopt for any other line.
[[nodiscard]] std::optional<Progress> parse_stats_line(std::string_view line);
// The message of an error-level JSON log line; nullopt otherwise.
[[nodiscard]] std::optional<std::string> parse_error_line(std::string_view line);

struct DownloadResult {
    int exit_code = 0;      // rclone's; proc::exit_stopped when cancelled
    std::string last_error; // last error-level message rclone logged, if any
};

// rclone's temporary files for these local files, as they are on disk now: "<name>.<8 hex>.partial"
// next to each (a name over 100 bytes is first cut short so the whole keeps its length). A
// stopped rclone leaves them behind.
[[nodiscard]] std::vector<std::filesystem::path> rclone_temp_files(std::span<const std::filesystem::path> files);
// Deletes those that weren't there before the run (`before`, from rclone_temp_files) and aren't
// files in their own right (`keep`: a folder's own files, which can be named like them, e.g.
// "notes.txt.12345678.partial"): only what that run of rclone made. Returns how many.
std::size_t remove_rclone_temp_files(std::span<const std::filesystem::path> files,
                                     std::span<const std::filesystem::path> before,
                                     const std::function<bool(const std::filesystem::path&)>& keep);

// Downloads with progress callbacks instead of a console progress bar (for the GUI). A
// cancelled download's partial files are removed. `on_copied` gets each file rclone reports
// copied (relative name); that needs rclone's info level (-v in `extra`). `local_name`: the
// name in `dest_dir` instead of the server's.
[[nodiscard]] std::expected<DownloadResult, std::string>
download(const Context& ctx, Mode mode, const std::string& remote_path,
         const std::filesystem::path& dest_dir, const std::function<void(const Progress&)>& on_progress,
         const proc::RunOptions& run = {}, std::span<const std::string> extra = {},
         const CommandHook& on_command = {}, const std::function<void(const std::string&)>& on_copied = {},
         const std::optional<std::filesystem::path>& local_name = {});

} // namespace grab::engine
