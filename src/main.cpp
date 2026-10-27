// grab: locate a file or folder on a Linux server by name (ssh + find), then download it with
// grab's resumable engine (cli_download.hpp), or with plain rclone (--rclone). See README.md.

#include "cli.hpp"
#include "cli_download.hpp"
#include "config.hpp"
#include "conn_budget.hpp"
#include "engine.hpp"
#include "existing.hpp"
#include "fetch.hpp"
#include "file_list.hpp"
#include "folder_job.hpp"
#include "gui_state.hpp"
#include "ini.hpp"
#include "local_name.hpp"
#include "match.hpp"
#include "process.hpp"
#include "quote.hpp"
#include "rate.hpp"
#include "rclone.hpp"
#include "rclone_flags.hpp"
#include "remote.hpp"
#include "schedule.hpp"
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

// --rclone: one `rclone copyto` / `rclone copy` per item with rclone's own progress, as before
// grab had its own download engine. Ctrl+C goes to rclone.
DownloadTally download_with_rclone(const engine::Context& ctx, const Options& opts,
                                   const std::vector<std::string>& paths, const std::filesystem::path& dest) {
    // --limit becomes rclone's --bwlimit, placed before anything given after `--` (which wins).
    // Without it, rclone follows the speed schedule by itself, as a weekly timetable.
    std::vector<std::string> extra;
    const auto gui = window_settings(state_dir(opts.config.value_or(default_grab_config_path())) / "gui.json");
    if (opts.limit) {
        if (*opts.limit > 0) extra.insert(extra.end(), {"--bwlimit", rate::bwlimit_arg(*opts.limit)});
    } else if (!gui.speed_schedule.empty()) {
        extra.insert(extra.end(), {"--bwlimit", schedule::rclone_bwlimit(gui.speed_schedule)});
        if (auto note = schedule_note(gui.speed_schedule, schedule::local_now())) std::println(stderr, "grab: {}", *note);
    }
    // --verify (or the window's setting): rclone compares checksums after each transfer itself,
    // which grab's default --sftp-disable-hashcheck would switch off.
    if (opts.verify.value_or(gui.verify_downloads)) extra.emplace_back("--sftp-disable-hashcheck=false");
    extra.insert(extra.end(), opts.extra.begin(), opts.extra.end());

    DownloadTally tally;
    const std::size_t total = paths.size();
    // Picks of the same name get numbered ones (review G103).
    const auto targets = existing::pick_targets(dest, paths, opts.mode == Mode::folder);
    // Different files already where rclone would write (another size, or a time off): asked first,
    // all of them at once. Skip leaves them (--ignore-existing, which also keeps anything that
    // appears meanwhile); a file can be kept beside the new one, a folder can't (rclone copies it
    // under the server's names).
    existing::Choice choice = opts.existing.value_or(existing::Choice::ask);
    // Remote path -> the server's size and time when a different file is there; nullopt when it is
    // there but couldn't be compared.
    std::unordered_map<std::string, std::optional<std::pair<std::uint64_t, std::string>>> in_the_way;
    if (!opts.dry_run) {
        std::vector<ExistingFile> shown;
        std::size_t count = 0;
        auto add = [&](const std::string& path, std::string name, const std::filesystem::path& local, std::uint64_t size,
                       const std::string& modtime) {
            if (!existing::rclone_replaces(local, size, util::parse_rfc3339(modtime))) return;
            in_the_way.try_emplace(path, std::pair{size, modtime});
            if (++count <= 10) shown.push_back({std::move(name), local, size, util::parse_rfc3339_seconds(modtime)});
        };
        // A check that failed (a network hiccup, a refusal) can't tell: whatever is there is asked
        // about as not compared, never left to --ignore-existing unseen (review G183).
        auto uncompared = [&](const std::string& path, const std::filesystem::path& local, std::string why) {
            std::error_code ec;
            if (!std::filesystem::exists(local, ec)) return; // nothing there to replace
            in_the_way.try_emplace(path, std::nullopt);
            if (++count <= 10) {
                shown.push_back({.name = util::path_to_utf8(local.filename()), .local = local, .size = 0,
                                 .modified = std::nullopt, .uncompared = std::move(why)});
            }
        };
        // grab's own check before rclone runs: the connection flags only (rclone_flags.hpp), so a
        // -vP or a filter meant for the copy can't break it (G182, G183).
        auto flags = rclone_flags::connection(ctx.settings.common_flags);
        const auto given = rclone_flags::connection(opts.extra);
        flags.insert(flags.end(), given.begin(), given.end());
        for (std::size_t i = 0; i < total; ++i) {
            const auto& path = paths[i];
            const auto& target = targets[i];
            if (opts.mode == Mode::file) {
                const fetch::Source src{ctx.config.rclone, ctx.config.rclone_config, ctx.settings.rclone_remote, path, flags};
                const auto stat = fetch::stat_argv(src);
                if (opts.verbose) std::println(stderr, "+ {}", quote::display_cmdline(stat));
                proc::RunOptions quiet;
                quiet.detached = true;
                const auto ran = proc::run_capture(stat, quiet);
                if (!ran) {
                    uncompared(path, target, ran.error());
                } else if (ran->exit_code != 0) {
                    uncompared(path, target, server_ops::clean_rclone_error(ran->err));
                } else if (const auto remote = fetch::parse_stat(ran->out); !remote) {
                    uncompared(path, target, remote.error());
                } else if (!remote->is_dir) {
                    add(path, util::path_to_utf8(target.filename()), target, remote->size, remote->modtime);
                }
                continue;
            }
            folder::Job job;
            job.ctx = &ctx;
            job.remote_path = path;
            job.dest = dest;
            if (target != folder::local_dir(dest, path)) job.local_name = target.filename();
            job.extra = opts.extra;
            if (opts.verbose) job.on_command = [](const std::vector<std::string>& argv) { std::println(stderr, "+ {}", quote::display_cmdline(argv)); };
            folder::Tree tree;
            std::mutex m;
            if (auto listed = folder::list(job, tree, m, std::stop_token{}); !listed) {
                uncompared(path, target, listed.error());
                continue;
            }
            const std::string folder_name = util::path_to_utf8(target.filename());
            for (const auto& f : tree.files) {
                add(path, folder_name + "/" + f.path, target / local::relative(f.path), f.size, f.modtime);
            }
        }
        if (count > 0 && choice == existing::Choice::ask) {
            const auto answer = choose_existing(opts, shown, count, opts.mode == Mode::file);
            if (!answer) {
                tally.code = answer.error();
                tally.failed = paths;
                return tally;
            }
            choice = answer->choice;
        } else if (count > 0 && choice == existing::Choice::keep_both && opts.mode == Mode::folder) {
            if (const auto answer = choose_existing(opts, shown, count, false); !answer) {
                tally.code = answer.error();
                tally.failed = paths;
                return tally;
            }
        }
    }
    if (choice != existing::Choice::replace) extra.emplace_back("--ignore-existing");

    for (std::size_t i = 0; i < total; ++i) {
        const std::string& path = paths[i];
        const auto& target = targets[i];
        std::optional<std::filesystem::path> local_name;
        const bool renamed = target != (opts.mode == Mode::folder ? folder::local_dir(dest, path)
                                                                  : dest / local::relative(remote_basename(path)));
        if (renamed) local_name = target.filename();
        if (const auto way = in_the_way.find(path); way != in_the_way.end() && opts.mode == Mode::file) {
            if (choice == existing::Choice::skip) {
                std::println(stderr, "grab: skipped {}: your file is kept", util::path_to_utf8(target.filename()));
                ++tally.done;
                continue;
            }
            if (choice == existing::Choice::keep_both) {
                // A copy an earlier run kept that already is the server's file (review G112).
                if (const auto& server = way->second) {
                    if (const auto kept = existing::kept_copy(target, server->first, util::parse_rfc3339(server->second))) {
                        std::println(stderr, "grab: {} is already there as {}, nothing fetched",
                                     util::path_to_utf8(target.filename()), util::path_to_utf8(kept->filename()));
                        ++tally.done;
                        continue;
                    }
                }
                const auto second = existing::second_copy(target);
                if (!second) {
                    error("no free name for a second copy next to " + util::path_to_utf8(target));
                    if (tally.code == 0) tally.code = exit_download_failed;
                    tally.failed.push_back(path);
                    continue;
                }
                local_name = second->filename();
            }
        }
        const auto built = engine::download_argv(ctx, opts.mode, path, dest, extra, local_name);
        if (!built) {
            error(built.error());
            if (tally.code == 0) tally.code = exit_download_failed;
            tally.failed.push_back(path);
            continue;
        }
        const auto& rclone_argv = *built;
        if (opts.verbose || opts.dry_run) {
            std::println(stderr, "+ {}", quote::display_cmdline(rclone_argv));
        }
        if (opts.dry_run) continue;

        const std::string position = total > 1 ? std::format(" {}/{}", i + 1, total) : "";
        std::println(stderr, "grab: {}{} {}  ->  {}", mode_noun(opts.mode), position,
                     remote_spec(ctx.settings.rclone_remote, path),
                     util::path_to_utf8(local_name ? dest / *local_name : dest / local::relative(remote_basename(path))));
        auto code = proc::run_inherit(rclone_argv);
        if (!code) {
            error(code.error());
            tally.code = exit_config;
            tally.failed.push_back(path);
            break;
        }
        if (*code == 0) {
            ++tally.done;
            continue;
        }
        if (tally.code == 0) tally.code = *code;
        tally.failed.push_back(path);
        if (*code == proc::exit_stopped) {
            tally.stopped = i + 1 < total;
            break;
        }
    }
    return tally;
}

// "45 s", "12 min", "3 h", "2 days".
std::string age_text(std::int64_t seconds) {
    if (seconds < 90) return std::format("{} s", seconds);
    if (seconds < 90 * 60) return std::format("{} min", (seconds + 30) / 60);
    if (seconds < 36 * 3600) return std::format("{} h", (seconds + 1800) / 3600);
    return std::format("{} days", (seconds + 43200) / 86400);
}

// engine = grab: the server's pool, which the download after the search uses too, so the command
// logs in once; a walk takes up to 8 of the server's connections. With -v, the way chosen is said
// as `said` + its lookup_text.
std::expected<engine::GrabSide, std::string> grab_side(const engine::Context& ctx, const Options& opts,
                                                       ssh::Pools& pools, std::string said) {
    auto target = engine::ssh_target(ctx);
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
    const bool on_grab = ctx.engine == Engine::grab;
    if (!file_list::can_answer(ctx, req)) {
        if (on_grab) {
            auto side = grab_side(ctx, opts, pools, std::format("looking up '{}' on {}", opts.target, ctx.settings.name));
            if (!side) return util::fail(side.error());
            req.grab = std::move(*side);
            if (opts.verbose && is_path_target(req.target)) {
                std::println(stderr, "grab: looking up '{}' on {} with one SFTP stat", opts.target, ctx.settings.name);
            }
        } else if (opts.verbose) {
            std::println(stderr, "grab: looking up '{}' via {}", opts.target,
                         ctx.method == FindMethod::ssh ? "ssh + find" : "rclone lsf");
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
    engine::GrabSide side;
    if (on_grab) {
        auto made = grab_side(ctx, opts, pools, std::format("the file list of {} comes", ctx.settings.name));
        if (!made) return util::fail(made.error());
        side = std::move(*made);
    }
    auto fetched = file_list::fetch(ctx, now, false, {}, trace, {}, side);
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
        std::println(stderr, "grab: imported {} from rclone.conf into {} (one time)",
                     util::join(ctx->imported, ", "), util::path_to_utf8(grab_rclone_config_path()));
    }
    // grab's own client takes --bind, --contimeout and --timeout of them (review G189).
    if (opts.verbose && ctx->engine == Engine::grab) {
        if (const auto unused = engine::unused_flags(*ctx); !unused.empty()) {
            std::println(stderr, "grab: [{}] common_flags grab's own SSH client doesn't use: {}", ctx->settings.name,
                         util::join(unused, " "));
        }
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
    // grab's own SSH connections (engine = grab), for the search and the download after it, so
    // `grab -s x DEST` logs in once; closed when the command ends.
    std::optional<ssh::Pools> pools(std::in_place);
    auto found = search(*ctx, req, opts, trace, *pools);
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
    // rclone's downloads open connections of their own: the search's, idle, aren't kept beside them.
    if (opts.rclone) pools.reset();
    const DownloadTally tally = opts.rclone ? download_with_rclone(*ctx, opts, *chosen, dest)
                                            : download_with_grab(*ctx, opts, *chosen, sizes, modified, dest, *pools);
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
