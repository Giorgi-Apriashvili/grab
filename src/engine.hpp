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
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace grab::ssh {
// ssh_find.hpp, which includes this file by way of file_list.hpp.
struct FindSpec;
struct FindOptions;
struct Found;
} // namespace grab::ssh

namespace grab::engine {

// Everything needed to talk to one server.
struct Context {
    GrabConfig config;
    RemoteSettings settings;
    RcloneRemote remote;  // connection details from grab.conf (server_conf::to_remote)
    std::vector<std::string> imported; // servers the import moved from rclone.conf into grab.conf just now
    GrabMethod grab_method = GrabMethod::probe; // how searches and lists run
    server_conf::Connection connection; // the server's grab.conf section
    std::filesystem::path passwords;    // grab's store of this configuration (passwords::path_in)
    std::vector<std::string> import_notes; // that import's notes, "[server] ..."
    // What keeps grab off the server (guard.hpp), from its settings, grab's store and the
    // environment, read when the context was made.
    guard::Settings guard;
};

// The Context of `settings`, a server of `config` whose grab.conf section holds `connection`, in
// the configuration at `grab_conf` (whose state_dir keeps its passwords). The guard reads grab's
// store (one decryption) for the server's rclone extras; a server it has a reason for is refused
// wherever grab would connect to it (ssh_target), as there is nothing left to send it to.
[[nodiscard]] Context make_context(GrabConfig config, RemoteSettings settings, server_conf::Connection connection,
                                   const std::filesystem::path& grab_conf);

// grab.conf + the selected server's section. Errors are ready to show. With `import`, first the
// one-time move of servers from rclone.conf into grab.conf (rclone_import::apply), which takes
// the settings lock when a section waits for it; the window's UI thread passes false.
[[nodiscard]] std::expected<Context, std::string>
load_context(const std::filesystem::path& config_path, const std::optional<std::string>& remote_name,
             bool import = true);

// Whether grab may reach the context's server: grab's known_hosts has a key for its [host]:port
// (server_conf::pinned, read now). Else "host key not trusted: use Trust", before grab's client
// dials (Main's fail2ban counts each handshake cut short). ssh_target asks it.
[[nodiscard]] std::expected<void, std::string> check_pinned(const Context& ctx);

// What keeps grab off the context's server (guard.hpp): none for most servers.
[[nodiscard]] std::vector<guard::Reason> grab_blockers(const Context& ctx);

// A server's connection settings for one run (RCLONE-REPLACEMENT.md, phase 8 step 3 and phase 9
// step 7): grab.conf's bind, connect_timeout and io_timeout, then the command's --bind,
// --contimeout and --timeout (`given`), which win; a blank --bind clears the server's bind. The bind
// with the setting it came from, for messages ("bind in [Main]", "--bind"). A bind left in an old
// key with no `bind` key (old_bind: common_flags' or folder_flags' --bind, ssh_options' -o
// BindAddress or -b) is an error that says to write it as `bind`: grab doesn't connect to the
// server rather than connect from another address (a VPN or a firewall rule may count on it), so
// every connection refuses it, Trust's and Test's too. The old keys' --contimeout and --timeout
// aren't read (a notice says what to write instead).
struct ConnectSettings {
    std::optional<std::string> bind;
    std::string bind_from;
    std::optional<std::chrono::milliseconds> connect_timeout; // 0: off
    std::optional<std::chrono::milliseconds> io_timeout;
};
[[nodiscard]] std::expected<ConnectSettings, std::string> connect_settings(const RemoteSettings& settings,
                                                                           const ConnectOptions& given = {});

// How grab's own SSH client logs in to the context's server: its grab.conf section, with the
// secrets grab's store keeps for exactly that login (ssh::target_for), once nothing keeps grab off
// it (grab_blockers; the error names the setting and how to clear it, guard::text), its
// connection settings are usable (connect_settings, with the command's own `given`) and
// check_pinned passes.
[[nodiscard]] std::expected<ssh::Target, std::string> ssh_target(const Context& ctx, const ConnectOptions& given = {});

// A search, list or Browse through grab's own SSH client: find over an exec channel where the
// server's check says it works, else the SFTP walk (RCLONE-REPLACEMENT.md, phase 4).
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
    GrabSide grab;
};

struct Hit {
    std::string path;                  // remote path, in grab's form (rclone_enc::grab_path)
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

// Called with each step grab's client takes on the server just before it does it ("sftp stat
// <path>", "exec <find command>", "sftp walk <roots>"), for --verbose and the window's details.
using CommandHook = std::function<void(const std::vector<std::string>&)>;

// The remote's search roots; "" (the login home) when none are set.
[[nodiscard]] std::vector<std::string> search_roots(const Context& ctx);

// A search's result from the entries found: duplicates dropped, ranked best first
// (rank_matches), sizes and dates kept.
[[nodiscard]] SearchResult make_result(const Context& ctx, const SearchRequest& req, std::vector<RemoteEntry> entries);

// Looks the target up through grab's own client: a path target is one SFTP STAT; a name search
// runs find or the walk (req.grab). An error means the lookup itself failed; no matches is a
// successful, empty result. Only `run.stop` of `run` counts.
[[nodiscard]] std::expected<SearchResult, std::string>
search(const Context& ctx, const SearchRequest& req, const proc::RunOptions& run = {},
       const CommandHook& on_command = {});
// Folders and files at once (the window's searches), `req.mode` aside: one find or one walk lists
// both kinds, where a search per kind walked the server twice (review G197).
[[nodiscard]] std::expected<std::pair<SearchResult, SearchResult>, std::string>
search_both(const Context& ctx, const SearchRequest& req, const proc::RunOptions& run = {},
            const CommandHook& on_command = {});

// The pool a lookup uses, `side`'s, else one made for it (of 4 connections).
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

} // namespace grab::engine
