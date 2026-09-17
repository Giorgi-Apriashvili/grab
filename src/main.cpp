// grab: locate a file or folder on a Linux server by name (ssh + find), then download it
// with rclone using mode-specific flags. See README.md.

#include "cli.hpp"
#include "config.hpp"
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
constexpr int exit_ssh = 4;

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

int do_init(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        std::println("config already exists: {}", util::path_to_utf8(path));
        return exit_ok;
    }
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error(std::format("cannot create {}: {}", util::path_to_utf8(path.parent_path()),
                          ec.message()));
        return exit_config;
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        error(std::format("cannot write {}", util::path_to_utf8(path)));
        return exit_config;
    }
    out << example_config;
    std::println("wrote example config to {}", util::path_to_utf8(path));
    std::println("edit search_roots / default_remote, then run e.g.  grab -f NAME E:\\Backup\\NAME");
    return exit_ok;
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
    FindRequest req;
    req.mode = opts.mode;
    req.target = opts.target;
    req.roots = settings.search_roots;
    req.max_depth = opts.depth.value_or(settings.max_depth);
    req.skip_hidden = settings.skip_hidden;

    if (!is_absolute_target(req.target) && req.roots.empty()) {
        error(std::format("[{}] search_roots is not set; add it to grab.conf or pass an absolute "
                          "remote path",
                          settings.name));
        return exit_config;
    }

    const auto ssh_argv =
        build_ssh_argv(cfg->ssh, *conn, settings.ssh_options, build_find_command(req));
    if (opts.verbose) std::println(stderr, "+ {}", quote::display_cmdline(ssh_argv));

    auto found = proc::run_capture(ssh_argv);
    if (!found) {
        error(found.error());
        return exit_ssh;
    }
    if (found->exit_code == 255) {
        error(std::format("ssh to {}@{}:{} failed (exit 255); see the message above", conn->user,
                          conn->host, conn->port));
        return exit_ssh;
    }

    auto matches = parse_find_output(found->out);
    if (matches.empty()) {
        if (is_absolute_target(req.target)) {
            error(std::format("no {} at '{}' on {}", mode_noun(req.mode), req.target, conn->host));
        } else {
            error(std::format("no {} named '{}' under {} (max depth {}) on {}", mode_noun(req.mode),
                              req.target, util::join(req.roots, ", "), req.max_depth, conn->host));
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
                 remote_spec(settings.rclone_remote, *chosen), util::path_to_utf8(dest));
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
    case CliAction::run:
        return run(parsed->opts);
    }
    return exit_usage;
}
