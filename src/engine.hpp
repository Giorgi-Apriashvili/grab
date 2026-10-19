#pragma once

// The steps behind both front ends (CLI and GUI): load a remote's context, search it, and
// download what was picked. Nothing here reads stdin or writes to the console.

#include "cli.hpp"
#include "config.hpp"
#include "process.hpp"
#include "remote.hpp"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace grab::engine {

// Everything needed to talk to one remote.
struct Context {
    GrabConfig config;    // rclone and rclone_config resolved (bundled exe, grab's own file)
    RemoteSettings settings;
    RcloneRemote remote;  // connection details from rclone.conf
    FindMethod method;    // resolved: ssh or rclone
    std::vector<std::string> imported; // remotes imported into grab's rclone.conf just now
};

// grab.conf + the selected remote section + its rclone.conf entry. Errors are ready to show.
// Also performs the one-time import of referenced remotes into grab's own rclone.conf.
[[nodiscard]] std::expected<Context, std::string>
load_context(const std::filesystem::path& config_path, const std::optional<std::string>& remote_name);

struct SearchRequest {
    std::string target; // words, glob, or a remote path containing '/'
    Mode mode = Mode::file;
    bool exact = false;
    std::optional<int> depth; // overrides the remote's max_depth
    bool batch_ssh = false;   // never let ssh prompt (GUI): -o BatchMode=yes
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
// (ssh or rclone exited non-zero); no matches is a successful, empty result.
[[nodiscard]] std::expected<SearchResult, std::string>
search(const Context& ctx, const SearchRequest& req, const proc::RunOptions& run = {},
       const CommandHook& on_command = {});

// rclone argv for downloading `remote_path` into `dest_dir` (DEST\<name>), with the remote's
// flags as configured (including -P for the console).
[[nodiscard]] std::vector<std::string> download_argv(const Context& ctx, Mode mode,
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
// Flags with console-only progress options removed (-P, --progress, --stats-one-line*, -q).
[[nodiscard]] std::vector<std::string> without_console_progress(std::span<const std::string> flags);

struct DownloadResult {
    int exit_code = 0;      // rclone's; proc::exit_stopped when cancelled
    std::string last_error; // last error-level message rclone logged, if any
};

// rclone's temporary files for these local files, as they are on disk now: "<name>.<8 hex>.partial"
// next to each (a name over 100 bytes is first cut short so the whole keeps its length). A
// stopped rclone leaves them behind.
[[nodiscard]] std::vector<std::filesystem::path> rclone_temp_files(std::span<const std::filesystem::path> files);
// Deletes those that weren't there before the run (`before`, from rclone_temp_files): only what
// that run of rclone made. Returns how many.
std::size_t remove_rclone_temp_files(std::span<const std::filesystem::path> files,
                                     std::span<const std::filesystem::path> before);

// Downloads with progress callbacks instead of a console progress bar (for the GUI). A
// cancelled download's partial files are removed. `on_copied` gets each file rclone reports
// copied (relative name); that needs rclone's info level (-v in `extra`).
[[nodiscard]] std::expected<DownloadResult, std::string>
download(const Context& ctx, Mode mode, const std::string& remote_path,
         const std::filesystem::path& dest_dir, const std::function<void(const Progress&)>& on_progress,
         const proc::RunOptions& run = {}, std::span<const std::string> extra = {},
         const CommandHook& on_command = {}, const std::function<void(const std::string&)>& on_copied = {});

} // namespace grab::engine
