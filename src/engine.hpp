#pragma once

// The steps behind both front ends (CLI and GUI): load a remote's context, search it, and
// download what was picked. Nothing here reads stdin or writes to the console.

#include "cli.hpp"
#include "config.hpp"
#include "guard.hpp"
#include "process.hpp"
#include "remote.hpp"
#include "server_conf.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace grab::ssh {
// ssh_find.hpp, which includes this file by way of file_list.hpp.
struct FindSpec;
struct FindOptions;
struct Found;
} // namespace grab::ssh

namespace grab::engine {

// Everything needed to talk to one remote.
struct Context {
    GrabConfig config;    // rclone resolved (the bundled exe)
    RemoteSettings settings;
    RcloneRemote remote;  // connection details from grab.conf (server_conf::to_remote)
    FindMethod method;    // resolved: ssh or rclone (engine = rclone)
    std::vector<std::string> imported; // servers the import moved from rclone.conf into grab.conf just now
    Engine engine = Engine::grab;      // effective_engine(settings, guarded): the guard keeps a server on rclone
    GrabMethod grab_method = GrabMethod::probe; // how searches and lists run on engine = grab
    server_conf::Connection connection; // the server's grab.conf section
    std::filesystem::path passwords;    // grab's store of this configuration (passwords::path_in)
    std::vector<std::string> import_notes; // that import's notes, "[server] ..."
    // What keeps grab's own client off the server (guard.hpp), from its settings, grab's store
    // and the environment, read when the context was made; grab_blockers adds a command's `--`.
    guard::Settings guard;
};

// The Context of `settings`, a server of `config` (rclone resolved) whose grab.conf section holds
// `connection`, in the configuration at `grab_conf` (whose state_dir keeps its passwords). The
// guard reads grab's store (one decryption) for the server's rclone extras. Its engine: GRAB_ENGINE,
// else its `engine` line, else grab's own client unless the guard has a reason to keep it off,
// which leaves it on rclone (effective_engine). A route rclone takes (guard::routed) makes an
// engine = rclone server search with rclone's listing, since ssh.exe find would connect past it.
[[nodiscard]] Context make_context(GrabConfig config, RemoteSettings settings, server_conf::Connection connection,
                                   const std::filesystem::path& grab_conf);

// grab.conf + the selected server's section. Errors are ready to show. With `import`, first the
// one-time move of servers from rclone.conf into grab.conf (rclone_import::apply), which takes
// the settings lock when a section waits for it; the window's UI thread passes false. rclone is
// checked (check_rclone) only for an engine = rclone server.
[[nodiscard]] std::expected<Context, std::string>
load_context(const std::filesystem::path& config_path, const std::optional<std::string>& remote_name,
             bool import = true);

// Whether grab may reach the context's server: grab's known_hosts has a key for its [host]:port
// (server_conf::pinned, read now). Else "host key not trusted: use Trust", before any rclone
// process starts or grab's client or ssh dials (RCLONE-REPLACEMENT.md, phase 7: rclone dials 10
// times per failed handshake, which Main's fail2ban counts). rclone_env, ssh_target and
// run_over_ssh ask it.
[[nodiscard]] std::expected<void, std::string> check_pinned(const Context& ctx);

// What keeps grab's own client off the context's server for a run whose connection flags after
// `--` are `run_flags` (guard.hpp): none for most servers.
[[nodiscard]] std::vector<guard::Reason> grab_blockers(const Context& ctx, std::span<const std::string> run_flags = {});

// A server's connection settings for one run (RCLONE-REPLACEMENT.md, phase 8 step 3): grab.conf's
// bind, connect_timeout and io_timeout; for each one not set, common_flags' --bind, --contimeout
// or --timeout, read as it; then the command's connection flags after `--` (`run_flags`), which
// win. Each with the setting it came from, for messages ("bind in [Main]", "common_flags'
// --bind", "--bind"). An error names the setting that can't be read, or a --bind that isn't an
// address (server_conf::check_host) before any program gets it.
struct ConnectSettings {
    std::optional<std::string> bind;
    std::string bind_from;
    std::optional<std::chrono::milliseconds> connect_timeout; // 0: off
    std::optional<std::chrono::milliseconds> io_timeout;
    std::string io_from; // "[Main] common_flags: --timeout off", for an error
    std::vector<std::string> legacy; // the keys common_flags gave: "bind", "connect_timeout", "io_timeout"
};
[[nodiscard]] std::expected<ConnectSettings, std::string> connect_settings(const RemoteSettings& settings,
                                                                           std::span<const std::string> run_flags = {});
// The keys that are set as rclone flags (--bind, --contimeout, --timeout), for every rclone run on
// the server after grab's fixed values (common_flags' come before them): nothing for blank ones,
// so rclone keeps its own times.
[[nodiscard]] std::vector<std::string> key_flags(const RemoteSettings& settings);
// The same for ssh.exe, before any of the server's ssh_options (OpenSSH keeps the first value):
// -o BindAddress=, -o ConnectTimeout= in whole seconds rounded up, none for off.
[[nodiscard]] std::vector<std::string> ssh_connect_options(const ConnectSettings& reach);

// How grab's own SSH client logs in to the context's server: its grab.conf section, with the
// secrets grab's store keeps for exactly that login (ssh::target_for), once check_pinned passes
// and nothing keeps grab's client off it (grab_blockers, with `extra`; the error says why and what
// that means on its engine). Its connection settings as connect_settings has them with `extra`,
// the command's connection flags after `--`; an I/O time outside 1 s to 24 h is refused (with no
// limit a stalled read couldn't be cancelled). On engine = grab its failures that rclone might get
// past name the fallback, engine = rclone in its section (ssh::Target::fallback).
[[nodiscard]] std::expected<ssh::Target, std::string> ssh_target(const Context& ctx,
                                                                 std::span<const std::string> extra = {});

// The command's flags after `--` (`extra`) for a run on the context's server, checked once its
// engine is known and before anything connects (RCLONE-REPLACEMENT.md, phase 8 step 6). None may
// replace the server grab sets itself (rclone_flags::first_refused). grab's own client (engine =
// grab) takes only --bind, --contimeout and --timeout, with values it can use (an address, times,
// a --timeout from 1 s to 24 h), and refuses the rest naming the setting to use instead
// (rclone_flags::first_unused_by_grab); engine = rclone takes connection flags
// (rclone_flags::first_unusable). The error is ready to show.
[[nodiscard]] std::expected<void, std::string> check_run_flags(const Context& ctx, std::span<const std::string> extra);

// What an rclone run on the context's server gets in its environment, its config in memory
// (`--config NUL`, config_in_memory; RCLONE-REPLACEMENT.md phase 7, decision 1), as
// RCLONE_CONFIG_<NAME>_<KEY> (secrets::env_name): TYPE sftp, HOST, PORT, USER, the login (agent:
// KEY_USE_AGENT and its KEY_FILE; key: KEY_FILE and KEY_FILE_PASS; password: PASS; stored_key:
// KEY_PEM as stored and KEY_FILE_PASS; a password saved beside a key too, which rclone tries after it), grab's
// known_hosts as KNOWN_HOSTS_FILE, SHELL_TYPE, MD5SUM_COMMAND and SHA1SUM_COMMAND ("none" when
// grab.conf doesn't know them, so rclone doesn't ask the server on every run), PATH_OVERRIDE,
// the section's rclone.<key> lines and the store's rclone extras. Secrets come from grab's store
// for exactly that login, PASS and KEY_FILE_PASS obscured as rclone takes them (secrets::obscure).
// A path holding '$' (rclone expands its key_file, pubkey_file and known_hosts_file again) goes
// in its own GRAB_RCLONE_<KEY> variable, which the value names, since Go doesn't expand what it
// substituted. Only that run sees them, never grab itself nor another program it starts (review
// G7), nor another server's run (G84). An unpinned server (check_pinned), or one whose password
// isn't saved for its login, is an error, and so is a common_flags --bind that isn't an address
// (connect_settings) or a flag there that would replace these (rclone_flags::first_refused). A
// store that exists but can't be read is an error for every login method (phase 8): it may keep
// the server's proxy or ssh command, which rclone would otherwise go without, connecting directly.
[[nodiscard]] std::expected<proc::Environment, std::string> rclone_env(const Context& ctx);

// rclone's version from `rclone version`'s output ("rclone v1.75.1").
struct RcloneVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;
    auto operator<=>(const RcloneVersion&) const = default;
};
[[nodiscard]] std::optional<RcloneVersion> parse_rclone_version(std::string_view out);
// The oldest rclone grab works with: --sftp-connections (every folder batch) came in 1.67.
inline constexpr RcloneVersion min_rclone{1, 67, 0};
// Whether `rclone` can be used: the one grab ships always; another (grab.conf's `rclone =`
// when `configured`, else PATH's because the bundled one is missing) is asked its version once
// per process and refused below min_rclone (review G190). Asked only where rclone runs:
// load_context on engine = rclone.
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
    // The find check's answers kept across runs (find_memory). None: asked once per pool.
    ssh::FindMemory memory;
};

struct SearchRequest {
    std::string target; // words, glob, or a remote path containing '/'
    Mode mode = Mode::file;
    bool exact = false;
    std::optional<int> depth; // overrides the remote's max_depth
    bool batch_ssh = false;   // never let ssh prompt (GUI): -o BatchMode=yes
    // For a walk with rclone: its connections from the pool (fetch::Listing::flags).
    std::vector<std::string> walk_flags;
    // The command's connection flags after `--`: last on rclone's and ssh.exe's command lines.
    std::vector<std::string> run_flags;
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
// reports unreadable folders that way, and what it printed is still good. ssh gets the server's
// connection settings with `run_flags` (connect_settings, ssh_connect_options) before its
// ssh_options.
[[nodiscard]] std::expected<std::string, std::string>
run_over_ssh(const Context& ctx, const std::string& command, bool batch_ssh, const proc::RunOptions& run = {},
             const CommandHook& on_command = {}, std::span<const std::string> run_flags = {});

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
// once per pool, unless side.memory has its answer), told to side.on_lookup. "search cancelled"
// when stopped.
[[nodiscard]] std::expected<ssh::Lookup, std::string> grab_lookup(const Context& ctx, ssh::Pool& pool,
                                                                  const GrabSide& side, std::stop_token stop);
// find for a search or list on the way grab_lookup chose, shown to `on_command`. When it fails as
// it does where there's no find for grab (ssh::FindFailed::about_find) on an answer known from
// before (saved by an earlier run, or asked earlier in the program), the check is asked again at
// once (ssh::recheck_lookup) and `way` becomes its answer, told to side.on_lookup: find runs
// again, or nullopt says to walk. "search cancelled" when stopped.
[[nodiscard]] std::expected<std::optional<ssh::Found>, std::string>
grab_find(const Context& ctx, ssh::Pool& pool, const GrabSide& side, ssh::Lookup& way, const ssh::FindSpec& spec,
          const ssh::FindOptions& options, const CommandHook& on_command);
// The find check's answers for `target`'s login (user@host:port), kept in limits.json
// (`limits_file`, learned_limits_path) beside the learned connection limits, so the window and
// the command ask a server once a week rather than once per run (ssh::find_check_trust); a host,
// port or user changed is another login, asked again. `report` (may be empty) hears how each
// write went. `before`: gui.json's old "serverLimits", kept when an answer makes the file
// (save_find_check).
[[nodiscard]] ssh::FindMemory find_memory(const std::filesystem::path& limits_file, const ssh::Target& target,
                                          std::function<void(const std::expected<void, std::string>&)> report,
                                          std::map<std::string, int> before = {});
// The way chosen in words, after "searching" or "listing" (-v, the window): "with find (GNU
// find)", "by listing folders (a Storage Box has no find)", "by listing folders (its answer to
// grab's find check: ...)"; a saved answer with its date ("with find (GNU find, checked on
// 2026-10-27)"), one asked again with why.
[[nodiscard]] std::string lookup_text(const Context& ctx, const ssh::Lookup& way);

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
