// grab: locate a file or folder on a Linux server by name (ssh + find), then download it
// with rclone using mode-specific flags. See README.md.

#include "cli.hpp"
#include "config.hpp"
#include "engine.hpp"
#include "ini.hpp"
#include "match.hpp"
#include "process.hpp"
#include "quote.hpp"
#include "rclone.hpp"
#include "remote.hpp"
#include "server_cli.hpp"
#include "update.hpp"
#include "util.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <print>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)

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
        auto rclone = read_rclone_for_init(cfg->rclone_config.value_or(standard_rclone_config_path()));
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

    const auto rclone_path = standard_rclone_config_path();
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
    out.close();

    if (remotes.empty()) {
        std::println("wrote {} (no servers yet)", shown);
        std::println("add one with:  grab server add");
        return exit_ok;
    }
    // Copy them into grab's own rclone.conf; the standard one is never modified.
    std::vector<std::string> names;
    for (const auto& r : remotes) names.push_back(r.name);
    const auto imported = import_rclone_remotes(names, rclone_path, grab_rclone_config_path());
    if (!imported.error.empty()) error(imported.error);
    std::println("wrote {} with {} server(s) imported from rclone.conf:", shown, remotes.size());
    for (const auto& r : remotes) std::println("  [{}]  {}", r.name, describe_remote(r));
    std::println("ready: grab NAME [DEST]   (search_roots is blank = login home; narrow it with "
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

// Absolute, normalized DEST directory, created if missing (unless `create` is false, as for
// --dry-run, which must not touch the disk).
std::expected<std::filesystem::path, std::string> prepare_destination(const std::filesystem::path& in,
                                                                     bool create = true) {
    std::error_code ec;
    auto dest = std::filesystem::absolute(in, ec);
    if (ec) return util::failf("bad destination '{}': {}", util::path_to_utf8(in), ec.message());
    dest = dest.lexically_normal();
    if (std::filesystem::exists(dest, ec) && !std::filesystem::is_directory(dest, ec)) {
        return util::failf("destination '{}' exists and is not a directory", util::path_to_utf8(dest));
    }
    if (!create) return dest;
    std::filesystem::create_directories(dest, ec);
    if (ec) {
        return util::failf("cannot create destination '{}': {}", util::path_to_utf8(dest),
                           ec.message());
    }
    return dest;
}

int run(const Options& opts) {
    // 2. configuration -------------------------------------------------------------------
    const auto cfg_path = opts.config.value_or(default_grab_config_path());
    auto ctx = engine::load_context(cfg_path, opts.remote);
    if (!ctx) {
        error(ctx.error());
        std::error_code ec;
        if (!std::filesystem::exists(cfg_path, ec)) {
            std::println(stderr, "hint: run `grab --init` to create {}", util::path_to_utf8(cfg_path));
        }
        return exit_config;
    }
    if (!ctx->imported.empty()) {
        std::println(stderr, "grab: imported {} from rclone.conf into {} (one time)",
                     util::join(ctx->imported, ", "), util::path_to_utf8(grab_rclone_config_path()));
    }

    // destination: a given DEST is checked before the search so a typo fails fast; a missing
    // one is asked for after the pick, which needs a terminal.
    const bool interactive = util::stdin_is_tty();
    std::filesystem::path dest;
    if (opts.dest) {
        auto prepared = prepare_destination(*opts.dest, !opts.dry_run);
        if (!prepared) {
            error(prepared.error());
            return exit_config;
        }
        dest = std::move(*prepared);
    } else if (!interactive) {
        error("no DEST given and no terminal to ask for one; pass DEST, e.g. grab NAME E:\\Downloads");
        return exit_usage;
    }

    // 3. resolve the remote path ---------------------------------------------------------
    if (opts.verbose) {
        std::println(stderr, "grab: looking up '{}' via {}", opts.target,
                     ctx->method == FindMethod::ssh ? "ssh + find" : "rclone lsf");
    }
    engine::SearchRequest req;
    req.target = opts.target;
    req.mode = opts.mode;
    req.exact = opts.exact;
    req.depth = opts.depth;
    const engine::CommandHook trace = [&](const std::vector<std::string>& argv) {
        if (opts.verbose) std::println(stderr, "+ {}", quote::display_cmdline(argv));
    };
    auto found = engine::search(*ctx, req, {}, trace);
    if (!found) {
        error(found.error());
        return exit_lookup;
    }

    const Query query = make_query(opts.target, opts.exact);
    if (found->hits.empty()) {
        if (is_path_target(opts.target)) {
            error(std::format("no {} at '{}' on {}", mode_noun(opts.mode), opts.target,
                              ctx->remote.host));
        } else {
            error(std::format("no {} {} '{}' under {} (max depth {}) on {}", mode_noun(opts.mode),
                              query.kind == Query::Kind::words ? "matching" : "named", opts.target,
                              describe_roots(found->roots), found->max_depth, ctx->remote.host));
        }
        return exit_not_found;
    }

    std::vector<std::string> paths;
    std::unordered_map<std::string, std::uint64_t> sizes;
    for (const auto& hit : found->hits) {
        paths.push_back(hit.path);
        if (hit.size) sizes.emplace(hit.path, *hit.size);
    }
    PickOptions pick;
    pick.first = opts.first;
    pick.all = opts.all;
    pick.interactive = interactive;
    auto chosen = choose_matches(std::move(paths), query, pick, std::cin, std::cerr,
                                 [&](const std::string& path) {
                                     const auto it = sizes.find(path);
                                     return it == sizes.end()
                                                ? std::string{}
                                                : std::format("  ({})", util::format_size(it->second));
                                 });
    if (!chosen) {
        error(chosen.error());
        return exit_not_found;
    }

    if (!opts.dest) {
        std::error_code cwd_ec;
        const auto cwd = std::filesystem::current_path(cwd_ec);
        for (int attempt = 0;; ++attempt) {
            auto answer = ask_destination(std::cin, std::cerr, cwd);
            if (!answer) {
                error(answer.error());
                return exit_usage;
            }
            auto prepared = prepare_destination(*answer, !opts.dry_run);
            if (prepared) {
                dest = std::move(*prepared);
                break;
            }
            error(prepared.error());
            if (attempt == 2) return exit_config;
        }
    }

    // 4 + 5. one rclone run per chosen item ----------------------------------------------
    const std::size_t total = chosen->size();
    std::size_t done = 0;
    int first_failure = 0;
    std::vector<std::string> failed;
    bool stopped = false;

    for (std::size_t i = 0; i < total; ++i) {
        const std::string& path = (*chosen)[i];
        const auto rclone_argv = engine::download_argv(*ctx, opts.mode, path, dest, opts.extra);
        if (opts.verbose || opts.dry_run) {
            std::println(stderr, "+ {}", quote::display_cmdline(rclone_argv));
        }
        if (opts.dry_run) continue;

        const std::string position = total > 1 ? std::format(" {}/{}", i + 1, total) : "";
        std::println(stderr, "grab: {}{} {}  ->  {}", mode_noun(opts.mode), position,
                     remote_spec(ctx->settings.rclone_remote, path),
                     util::path_to_utf8(dest / util::path_from_utf8(remote_basename(path))));
        auto code = proc::run_inherit(rclone_argv);
        if (!code) {
            error(code.error());
            return exit_config;
        }
        if (*code == 0) {
            ++done;
            continue;
        }
        if (first_failure == 0) first_failure = *code;
        failed.push_back(path);
        if (*code == proc::exit_stopped) {
            stopped = i + 1 < total;
            break;
        }
    }
    if (opts.dry_run) return exit_ok;

    if (total > 1) {
        std::string summary = std::format("grab: {} of {} done", done, total);
        if (!failed.empty()) summary += std::format("; failed: {}", util::join(failed, ", "));
        if (stopped) summary += "; stopped after Ctrl+C";
        std::println(stderr, "{}", summary);
    }
    return first_failure;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    // A previous `grab update` left the replaced binary as grab.exe.old; it can be deleted
    // once that process has exited.
    if (auto self = util::self_exe_path(); !self.empty()) {
        // grab.exe, rclone.exe or grab-gui.exe replaced by an update: delete once unused.
        std::error_code ignored;
        for (const auto& e : std::filesystem::directory_iterator(self.parent_path(), ignored)) {
            const auto name = e.path().filename().wstring();
            if (name.ends_with(L".exe.old")) std::filesystem::remove(e.path(), ignored);
        }
    }
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
    case CliAction::update:
        return update::run(parsed->opts.check);
    case CliAction::server:
        return run_server_command(parsed->opts);
    case CliAction::run:
        return run(parsed->opts);
    }
    return exit_usage;
}
