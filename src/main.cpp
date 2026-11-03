// grab: locate a file or folder on a Linux server by name (ssh + find), then download it with
// grab's resumable engine (cli_download.hpp). See README.md.

#include "cli.hpp"
#include "cli_download.hpp"
#include "config.hpp"
#include "conn_budget.hpp"
#include "engine.hpp"
#include "file_list.hpp"
#include "gui_state.hpp"
#include "ini.hpp"
#include "match.hpp"
#include "notices.hpp"
#include "process.hpp"
#include "quote.hpp"
#include "rclone_flags.hpp"
#include "rclone_import.hpp"
#include "remote.hpp"
#include "server_cli.hpp"
#include "server_ops.hpp"
#include "ssh_pool.hpp"
#include "update.hpp"
#include "util.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <iostream>
#include <map>
#include <optional>
#include <print>
#include <string>
#include <unordered_map>
#include <utility>
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
constexpr int exit_lookup = 4; // the search or the file list failed

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
        if (!cfg) {
            error(cfg.error());
            return exit_config;
        }
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

    if (auto written = util::write_file_atomic(path, text); !written) {
        error(written.error());
        return exit_config;
    }

    if (remotes.empty()) {
        std::println("wrote {} (no servers yet)", shown);
        std::println("add one with:  grab server add");
        return exit_ok;
    }
    // Their connection details into the new sections, their passwords into grab's store: the
    // import (rclone.conf is never modified). What an earlier grab.conf at this path imported
    // doesn't count for this new one.
    const auto files = rclone_import::files_for(path);
    std::filesystem::remove(rclone_import::record_path(files.state_dir), ec);
    const auto applied = rclone_import::apply(files);
    if (!applied) error(applied.error());
    // The lookups as runs resolve them now that the details and the store are in: in the comments
    // and below.
    std::map<std::string, std::string, std::less<>> searches;
    if (applied) {
        if (auto env = server_ops::load_env(path)) {
            if (auto described = server_ops::describe_servers(*env); !described) error(described.error());
            for (auto& info : server_ops::list_servers(*env)) searches[info.name] = std::move(info.search);
        }
    }
    std::println("wrote {} with {} server(s) imported from rclone.conf:", shown, remotes.size());
    for (const auto& r : remotes) {
        const auto found = searches.find(section_name_for(r.name));
        std::println("  [{}]  {}", r.name, describe_remote(r, found != searches.end() ? found->second : ""));
    }
    if (applied) {
        for (const auto& note : applied->notes) std::println("  note: [{}] {}", note.server, note.text);
    }
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

// "45 s", "12 min", "3 h", "2 days".
std::string age_text(std::int64_t seconds) {
    if (seconds < 90) return std::format("{} s", seconds);
    if (seconds < 90 * 60) return std::format("{} min", (seconds + 30) / 60);
    if (seconds < 36 * 3600) return std::format("{} h", (seconds + 1800) / 3600);
    return std::format("{} days", (seconds + 43200) / 86400);
}

// The server's pool, which the download after the search uses too, so the command logs in once; a
// walk takes up to 8 of the server's connections. With -v, the way chosen is said as `said` + its
// lookup_text.
std::expected<engine::GrabSide, std::string> grab_side(const engine::Context& ctx, const Options& opts,
                                                       ssh::Pools& pools, std::string said) {
    // The connection flags after `--` too, as the download's target takes them: one pool for both.
    auto target = engine::ssh_target(ctx, rclone_flags::connection(opts.extra));
    if (!target) return util::fail(target.error());
    // The download's budget, as download_with_grab counts it (limits.json: what refusals taught).
    const auto limits_file = learned_limits_path(opts.config.value_or(default_grab_config_path()));
    // gui.json's limits from grab 0.17 and older, while limits.json doesn't exist yet (review G195).
    // Read quietly: the download says so when gui.json can't be used.
    const auto gui = read_gui_state(limits_file.parent_path() / "gui.json");
    const auto old_limits = gui && *gui ? (*gui)->server_limits : std::map<std::string, int>{};
    const auto limits = load_learned_limits(limits_file, old_limits);
    const auto learned = limits.find(ctx.settings.name);
    const int budget = budget::effective_budget(ctx.settings.max_connections, budget::default_budget(ctx.remote.host),
                                                learned == limits.end() ? std::nullopt : std::optional(learned->second));
    engine::GrabSide side;
    side.pool = pools.get(ctx.settings.name, *target, budget);
    side.walk_seat = [budget] { return std::min(budget, 8); };
    // The find check as the window or an earlier command left it (limits.json too).
    side.memory = engine::find_memory(limits_file, *target, [](const std::expected<void, std::string>& saved) {
        if (!saved) std::println(stderr, "grab: {}", saved.error());
    }, old_limits);
    if (opts.verbose) {
        side.on_lookup = [&ctx, said = std::move(said)](const ssh::Lookup& way) {
            std::println(stderr, "grab: {} {}", said, engine::lookup_text(ctx, way));
        };
    }
    return side;
}

// The search, from the server's saved file list when it can answer it: the list is used while
// it is fresh, else fetched again first (and saved). Other searches go live.
std::expected<engine::SearchResult, std::string> search(const engine::Context& ctx, engine::SearchRequest req,
                                                        const Options& opts, const engine::CommandHook& trace,
                                                        ssh::Pools& pools) {
    if (!file_list::can_answer(ctx, req)) {
        auto side = grab_side(ctx, opts, pools, std::format("looking up '{}' on {}", opts.target, ctx.settings.name));
        if (!side) return util::fail(side.error());
        req.grab = std::move(*side);
        if (opts.verbose && is_path_target(req.target)) {
            std::println(stderr, "grab: looking up '{}' on {} with one SFTP stat", opts.target, ctx.settings.name);
        }
        return engine::search(ctx, req, {}, trace);
    }
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch()).count();
    const auto file = file_list::path_for(file_list::dir_for(opts.config.value_or(default_grab_config_path())),
                                          ctx.settings.name);
    auto list = opts.fresh ? std::nullopt : file_list::load(file);
    if (list && list->fingerprint == file_list::fingerprint(ctx) && file_list::fresh(*list, now)) {
        if (opts.verbose) {
            std::println(stderr, "grab: searching the file list of {} ({} old, {} entries)", ctx.settings.name,
                         age_text(now - list->fetched), list->entries.size());
        }
        return file_list::search(ctx, *list, req);
    }
    std::println(stderr, "grab: fetching the file list of {}...", ctx.settings.name);
    auto side = grab_side(ctx, opts, pools, std::format("the file list of {} comes", ctx.settings.name));
    if (!side) return util::fail(side.error());
    auto fetched = file_list::fetch(ctx, now, {}, trace, *side);
    if (!fetched) return util::fail(fetched.error());
    if (!file_list::save(file, *fetched) && opts.verbose) {
        std::println(stderr, "grab: could not save the file list to {}", util::path_to_utf8(file));
    }
    return file_list::search(ctx, *fetched, req);
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
        std::println(stderr, "grab: moved the connection details of {} from rclone.conf into {} (one time; rclone.conf "
                             "unchanged)",
                     util::join(ctx->imported, ", "), util::path_to_utf8(cfg_path));
    }
    for (const auto& note : ctx->import_notes) std::println(stderr, "grab: note: {}", note);
    // The settings' notices this configuration hasn't shown yet (notices.hpp); -v repeats the
    // server's. They name settings, never print a value (a proxy can hold a password).
    if (auto all = notices::for_config(cfg_path)) {
        auto fresh = notices::take_new(notices::path_in(state_dir(cfg_path)), *all);
        const auto& shown = fresh ? *fresh : *all; // unsaved: said every time rather than never
        for (const auto& n : shown) std::println(stderr, "grab: note: {}", n.text);
        if (opts.verbose) {
            for (const auto& n : *all) {
                if (n.server == ctx->settings.name && !std::ranges::contains(shown, n)) {
                    std::println(stderr, "grab: note: {}", n.text);
                }
            }
        }
    }
    // The flags after `--`: refused before anything connects or any folder is made
    // (RCLONE-REPLACEMENT.md, phase 8 step 6).
    if (auto usable = engine::check_run_flags(*ctx, opts.extra); !usable) {
        error(usable.error());
        return exit_usage;
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
    engine::SearchRequest req;
    req.target = opts.target;
    req.mode = opts.mode;
    req.exact = opts.exact;
    req.depth = opts.depth;
    const engine::CommandHook trace = [&](const std::vector<std::string>& argv) {
        if (opts.verbose) std::println(stderr, "+ {}", quote::display_cmdline(argv));
    };
    // grab's own SSH connections, for the search and the download after it, so `grab -s x DEST`
    // logs in once; closed when the command ends.
    ssh::Pools pools;
    auto found = search(*ctx, req, opts, trace, pools);
    if (!found) {
        error(found.error());
        return exit_lookup;
    }
    // What the lookup couldn't look at, in one line however many (review G55).
    if (const auto& notes = found->notes; !notes.empty()) {
        std::println(stderr, "grab: not searched: {}{}", notes.front(),
                     notes.size() > 1 ? std::format(" (and {} more)", notes.size() - 1) : std::string());
    }

    const Query query = make_query(opts.target, opts.exact);
    // --type, --min-size, --max-size: the hits that are such files.
    const std::size_t unfiltered = found->hits.size();
    if (opts.filter.active()) {
        std::erase_if(found->hits, [&](const engine::Hit& h) { return !opts.filter.matches(h.name, h.size); });
        if (found->hits.empty() && unfiltered > 0) {
            error(unfiltered == 1 ? std::format("1 file found, but it isn't {}", opts.filter.describe())
                                  : std::format("{} files found, but none is {}", unfiltered, opts.filter.describe()));
            return exit_not_found;
        }
    }
    if (found->hits.empty()) {
        if (is_path_target(opts.target)) {
            error(std::format("no {} at '{}' on {}", mode_noun(opts.mode), opts.target,
                              ctx->remote.host));
        } else {
            error(std::format("no {} {} '{}' under {} (max depth {}) on {}", mode_noun(opts.mode),
                              query.kind == Query::Kind::words ? "matching" : "named", opts.target,
                              search_roots_value(found->roots), found->max_depth, ctx->remote.host));
        }
        return exit_not_found;
    }

    std::vector<std::string> paths;
    std::unordered_map<std::string, std::uint64_t> sizes;
    std::unordered_map<std::string, std::int64_t> modified;
    std::unordered_map<std::string, const engine::Hit*> hit_at;
    for (const auto& hit : found->hits) {
        paths.push_back(hit.path);
        if (hit.size) sizes.emplace(hit.path, *hit.size);
        if (hit.modified) modified.emplace(hit.path, *hit.modified);
        hit_at.emplace(hit.path, &hit);
    }
    PickOptions pick;
    pick.first = opts.first;
    pick.all = opts.all;
    pick.interactive = interactive;
    auto chosen = choose_matches(std::move(paths), query, pick, std::cin, std::cerr,
                                 [&](const std::string& path) {
                                     const auto it = hit_at.find(path);
                                     return it == hit_at.end() ? std::string{}
                                                               : pick_note(it->second->size, it->second->modified);
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

    // 4 + 5. download each chosen item -------------------------------------------------
    const DownloadTally tally = download_with_grab(*ctx, opts, *chosen, sizes, modified, dest, pools);
    if (opts.dry_run) return exit_ok;

    if (const std::size_t total = chosen->size(); total > 1) {
        std::string summary = std::format("grab: {} of {} done", tally.done, total);
        if (!tally.failed.empty()) summary += std::format("; failed: {}", util::join(tally.failed, ", "));
        if (tally.stopped) summary += "; stopped after Ctrl+C";
        if (tally.quit) summary += "; quit at the question";
        if (tally.not_started > 0) summary += std::format(", {} not started", tally.not_started);
        std::println(stderr, "{}", summary);
    }
    return tally.code;
}

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    update::remove_replaced_programs();
#endif
    const auto args = collect_args(argc, argv);
    auto parsed = parse_args(args);
    if (!parsed) {
        error(parsed.error());
        std::print(stderr, "\n{}", usage());
        return exit_usage;
    }

    // A portable folder updated by v0.2.0's updater got grab.exe only; point at the fix.
    if (parsed->action != CliAction::help && parsed->action != CliAction::version &&
        parsed->action != CliAction::update) {
        if (auto note = update::incomplete_folder_note()) std::println(stderr, "grab: {}", *note);
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
