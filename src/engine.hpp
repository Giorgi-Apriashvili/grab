#pragma once

// The steps behind both front ends (CLI and GUI): load a remote's context, search it, and
// download what was picked. Nothing here reads stdin or writes to the console.

#include "cli.hpp"
#include "config.hpp"
#include "process.hpp"

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
    GrabConfig config;
    RemoteSettings settings;
    RcloneRemote remote;  // connection details from rclone.conf
    FindMethod method;    // resolved: ssh or rclone
};

// grab.conf + the selected remote section + its rclone.conf entry. Errors are ready to show.
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
};

struct SearchResult {
    std::vector<Hit> hits; // ranked best first (see rank_matches)
    std::vector<std::string> roots; // searched roots ("" = login home), for messages
    int max_depth = 0;
};

// Called with each external command just before it runs (for --verbose).
using CommandHook = std::function<void(const std::vector<std::string>&)>;

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
                                                     std::span<const std::string> extra = {});

// Live transfer state, parsed from rclone's --use-json-log stats lines.
struct Progress {
    std::uint64_t bytes = 0;
    std::uint64_t total = 0;
    double speed = 0;          // bytes per second
    std::optional<double> eta; // seconds
    std::string current;       // name of a file being transferred, if any
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

// Deletes rclone's temporary "<name>.<random>.partial" files left by an interrupted
// download of `local_target` (a file, or every *.partial inside a folder target). Returns
// how many were removed.
std::size_t remove_partials(const std::filesystem::path& local_target, Mode mode);

// Downloads with progress callbacks instead of a console progress bar (for the GUI). A
// cancelled download's partial files are removed.
[[nodiscard]] std::expected<DownloadResult, std::string>
download(const Context& ctx, Mode mode, const std::string& remote_path,
         const std::filesystem::path& dest_dir, const std::function<void(const Progress&)>& on_progress,
         const proc::RunOptions& run = {}, std::span<const std::string> extra = {},
         const CommandHook& on_command = {});

} // namespace grab::engine
