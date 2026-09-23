// grab: locate a file or folder on a Linux server by name (ssh + find), then download it
// with rclone using mode-specific flags. See README.md.

#include "cli.hpp"
#include "config.hpp"
#include "ini.hpp"
#include "listing.hpp"
#include "process.hpp"
#include "quote.hpp"
#include "rclone.hpp"
#include "remote.hpp"
#include "util.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>
#endif

namespace {

using namespace grab;

constexpr int exit_ok = 0;
constexpr int exit_usage = 1;
constexpr int exit_config = 2;
constexpr int exit_not_found = 3;
constexpr int exit_lookup = 4; // ssh or rclone listing failed

void error(std::string_view msg) { std::println(stderr, "grab: error: {}", msg); }

std::vector<std::string> collect_args([[maybe_unused]] int argc, [[maybe_unused]] char** argv) {
    std::vector<std::string> args;
#ifdef _WIN32
    // Take the arguments as UTF-16 and convert, so non-ASCII names survive regardless of
    // the console code page.
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv != nullptr) {
        for (int i = 1; i < wargc; ++i) args.push_back(util::to_utf8(wargv[i]));
        LocalFree(wargv);
    }
#else
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
#endif
    return args;
}

// rclone.conf for --init, or nullopt with the reason printed.
std::optional<ini::Document> read_rclone_for_init(const std::filesystem::path& p) {
    const auto shown = util::path_to_utf8(p);
    auto text = util::read_file(p);
    if (!text) {
        std::println("no rclone.conf at {}", shown);
        return std::nullopt;
    }
    if (is_encrypted_rclone_config(*text)) {
        std::println("{} is encrypted; grab cannot read its remotes", shown);
        return std::nullopt;
    }
    auto doc = ini::parse(*text);
    if (!doc) {
        std::println("cannot parse {}: {}", shown, doc.error());
        return std::nullopt;
    }
    return std::move(*doc);
}

int do_init(const std::filesystem::path& path) {
    std::error_code ec;
    const auto shown = util::path_to_utf8(path);

    if (std::filesystem::exists(path, ec)) {
        std::println("config already exists: {}", shown);
        // Never rewrite it; only point out rclone remotes it does not cover yet.
        auto cfg = load_grab_config(path);
        if (!cfg) return exit_ok;
        auto rclone = read_rclone_for_init(cfg->rclone_config.value_or(default_rclone_config_path()));
        if (!rclone) return exit_ok;
        const auto missing = missing_remotes(*cfg, *rclone);
        if (missing.empty()) return exit_ok;
        std::println("\nrclone.conf has sftp remotes grab.conf does not use yet. To add them, paste");
        std::println("these sections into the file (`grab config` opens it):\n");
        for (const auto& r : missing) std::println("{}", remote_section(r));
        return exit_ok;
    }

    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error(std::format("cannot create {}: {}", util::path_to_utf8(path.parent_path()),
                          ec.message()));
        return exit_config;
    }

    const auto rclone_path = default_rclone_config_path();
    const auto rclone = read_rclone_for_init(rclone_path);
    const auto remotes = rclone ? usable_sftp_remotes(*rclone) : std::vector<RcloneRemote>{};
    const auto text =
        generate_grab_config(rclone ? &*rclone : nullptr, util::path_to_utf8(rclone_path));

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error(std::format("cannot write {}", shown));
        return exit_config;
    }
    out << text;

    if (remotes.empty()) {
        if (rclone) std::println("no usable sftp remote in rclone.conf");
        std::println("wrote example config to {}", shown);
        std::println("set up the server with `rclone config`, then edit the example section "
                     "(`grab config`)");
        return exit_ok;
    }
    std::println("wrote {} with {} remote(s) from rclone.conf:", shown, remotes.size());
    for (const auto& r : remotes) std::println("  [{}]  {}", r.name, describe_remote(r));
    std::println("ready: grab -f NAME DEST   (search_roots is blank = login home; narrow it with "
                 "`grab config`)");
    return exit_ok;
}

int do_config(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (const int rc = do_init(path); rc != exit_ok) return rc;
    }

    // Read only the [grab] editor key, without validating the rest: a broken config file
    // must still be openable so it can be fixed.
    std::vector<std::string> candidates;
    if (auto doc = ini::parse_file(path)) {
        if (const auto* g = doc->find("grab")) candidates.push_back(g->get("editor").value_or(""));
    }
    candidates.push_back(util::getenv_utf8("VISUAL").value_or(""));
    candidates.push_back(util::getenv_utf8("EDITOR").value_or(""));

    const auto argv = editor_command(candidates, path);
    std::println(stderr, "grab: opening {} with {}", util::path_to_utf8(path), argv.front());
    auto code = proc::run_inherit(argv);
    if (!code) {
        error(code.error());
        std::println(stderr, "hint: set `editor =` in the [grab] section, or $VISUAL / $EDITOR");
        return exit_config;
    }
    return *code;
}

std::string describe_roots(const std::vector<std::string>& roots) {
    std::vector<std::string> shown;
    for (const auto& r : roots) shown.push_back(r.empty() ? "~" : r);
    return util::join(shown, ", ");
}

// 3a. ssh + find: one round trip, needs a shell on the server.
std::expected<std::vector<std::string>, std::string>
lookup_via_ssh(const Options& opts, const GrabConfig& cfg, const RemoteSettings& settings,
               const RcloneRemote& conn, const std::vector<std::string>& roots, int max_depth) {
    FindRequest req;
    req.mode = opts.mode;
    req.target = opts.target;
    req.roots = roots;
    req.max_depth = max_depth;
    req.skip_hidden = settings.skip_hidden;

    const auto argv = build_ssh_argv(cfg.ssh, conn, settings.ssh_options, build_find_command(req));
    if (opts.verbose) std::println(stderr, "+ {}", quote::display_cmdline(argv));

    auto res = proc::run_capture(argv);
    if (!res) return util::fail(res.error());
    if (res->exit_code == 255) {
        return util::failf("ssh to {}@{}:{} failed (exit 255); see the message above. If this "
                           "host has no shell (e.g. a storage box), set `find = rclone` in [{}]",
                           conn.user, conn.host, conn.port, settings.name);
    }
    return parse_find_output(res->out);
}

// 3b. rclone lsf: an SFTP walk per root, works without a shell and without prompts.
std::expected<std::vector<std::string>, std::string>
lookup_via_rclone(const Options& opts, const GrabConfig& cfg, const RemoteSettings& settings,
                  const std::vector<std::string>& roots, int max_depth) {
    // A path target is checked with a single listing of its parent, whatever the roots.
    const std::vector<std::string> bases =
        is_path_target(opts.target) ? std::vector<std::string>{""} : roots;

    std::vector<std::string> matches;
    for (const auto& root : bases) {
        ListRequest req;
        req.mode = opts.mode;
        req.target = opts.target;
        req.root = root;
        req.max_depth = max_depth;
        req.skip_hidden = settings.skip_hidden;
        req.rclone_exe = cfg.rclone;
        req.rclone_config = cfg.rclone_config;
        req.rclone_remote = settings.rclone_remote;

        const auto argv = build_lsf_argv(req);
        if (opts.verbose) std::println(stderr, "+ {}", quote::display_cmdline(argv));

        auto res = proc::run_capture(argv);
        if (!res) return util::fail(res.error());
        if (res->exit_code != 0) {
            return util::failf("rclone lsf of '{}' failed (exit {}); see the message above",
                               argv[2], res->exit_code);
        }
        auto found = parse_lsf_output(req, res->out);
        matches.insert(matches.end(), found.begin(), found.end());
    }
    return matches;
}

int run(const Options& opts) {
    // 2. configuration -------------------------------------------------------------------
    const auto cfg_path = opts.config.value_or(default_grab_config_path());
    auto cfg = load_grab_config(cfg_path);
    if (!cfg) {
        error(cfg.error());
        std::println(stderr, "hint: run `grab --init` to create {}", util::path_to_utf8(cfg_path));
        return exit_config;
    }
    auto remote = cfg->select(opts.remote);
    if (!remote) {
        error(remote.error());
        return exit_config;
    }
    const RemoteSettings& settings = **remote;

    const auto rclone_conf = cfg->rclone_config.value_or(default_rclone_config_path());
    auto conn = load_rclone_remote(rclone_conf, settings.rclone_remote);
    if (!conn) {
        error(conn.error());
        return exit_config;
    }

    // destination ------------------------------------------------------------------------
    std::error_code ec;
    auto dest = std::filesystem::absolute(opts.dest, ec);
    if (ec) {
        error(std::format("bad destination '{}': {}", util::path_to_utf8(opts.dest), ec.message()));
        return exit_config;
    }
    dest = dest.lexically_normal();
    if (std::filesystem::exists(dest, ec) && !std::filesystem::is_directory(dest, ec)) {
        error(std::format("destination '{}' exists and is not a directory", util::path_to_utf8(dest)));
        return exit_config;
    }
    std::filesystem::create_directories(dest, ec);
    if (ec) {
        error(std::format("cannot create destination '{}': {}", util::path_to_utf8(dest),
                          ec.message()));
        return exit_config;
    }

    // 3. resolve the remote path ---------------------------------------------------------
    const int max_depth = opts.depth.value_or(settings.max_depth);
    std::vector<std::string> roots = settings.search_roots;
    if (roots.empty()) roots.emplace_back(); // the login home

    const FindMethod method = resolve_find_method(settings, *conn);
    if (opts.verbose) {
        std::println(stderr, "grab: looking up '{}' via {}", opts.target,
                     method == FindMethod::ssh ? "ssh + find" : "rclone lsf");
    }
    auto lookup = method == FindMethod::ssh
                      ? lookup_via_ssh(opts, *cfg, settings, *conn, roots, max_depth)
                      : lookup_via_rclone(opts, *cfg, settings, roots, max_depth);
    if (!lookup) {
        error(lookup.error());
        return exit_lookup;
    }

    auto matches = std::move(*lookup);
    if (matches.empty()) {
        if (is_path_target(opts.target)) {
            error(std::format("no {} at '{}' on {}", mode_noun(opts.mode), opts.target,
                              conn->host));
        } else {
            error(std::format("no {} named '{}' under {} (max depth {}) on {}",
                              mode_noun(opts.mode), opts.target, describe_roots(roots), max_depth,
                              conn->host));
        }
        return exit_not_found;
    }

    auto chosen = choose_match(std::move(matches), opts.first, util::stdin_is_tty(), std::cin,
                               std::cerr);
    if (!chosen) {
        error(chosen.error());
        return exit_not_found;
    }

    // 4. rclone command ------------------------------------------------------------------
    RcloneRequest rq;
    rq.mode = opts.mode;
    rq.rclone_exe = cfg->rclone;
    rq.rclone_config = cfg->rclone_config;
    rq.rclone_remote = settings.rclone_remote;
    rq.remote_path = *chosen;
    rq.dest_dir = dest;
    rq.common_flags = settings.common_flags;
    rq.mode_flags = opts.mode == Mode::folder ? settings.folder_flags : settings.file_flags;
    rq.extra = opts.extra;
    const auto rclone_argv = build_rclone_argv(rq);

    if (opts.verbose || opts.dry_run) {
        std::println(stderr, "+ {}", quote::display_cmdline(rclone_argv));
    }
    if (opts.dry_run) return exit_ok;

    // 5. transfer ------------------------------------------------------------------------
    std::println(stderr, "grab: {} {}  ->  {}", mode_noun(opts.mode),
                 remote_spec(settings.rclone_remote, *chosen), util::path_to_utf8(local_target(rq)));
    auto code = proc::run_inherit(rclone_argv);
    if (!code) {
        error(code.error());
        return exit_config;
    }
    return *code;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    const auto args = collect_args(argc, argv);
    auto parsed = parse_args(args);
    if (!parsed) {
        error(parsed.error());
        std::print(stderr, "\n{}", usage());
        return exit_usage;
    }

    switch (parsed->action) {
    case CliAction::help:
        std::print("{}", usage());
        return exit_ok;
    case CliAction::version:
        std::println("grab {}", GRAB_VERSION);
        return exit_ok;
    case CliAction::init:
        return do_init(parsed->opts.config.value_or(default_grab_config_path()));
    case CliAction::config:
        return do_config(parsed->opts.config.value_or(default_grab_config_path()));
    case CliAction::run:
        return run(parsed->opts);
    }
    return exit_usage;
}
