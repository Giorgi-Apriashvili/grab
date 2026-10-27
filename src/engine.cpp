#include "engine.hpp"

#include "conn_budget.hpp"
#include "folder.hpp"
#include "gui_state.hpp"
#include "json.hpp"
#include "listing.hpp"
#include "match.hpp"
#include "rclone.hpp"
#include "rclone_encoding.hpp"
#include "rclone_flags.hpp"
#include "remote.hpp"
#include "ini.hpp"
#include "secrets.hpp"
#include "sftp.hpp"
#include "ssh_find.hpp"
#include "ssh_target.hpp"
#include "ssh_walk.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace grab::engine {

std::optional<RcloneVersion> parse_rclone_version(std::string_view out) {
    constexpr std::string_view mark = "rclone v";
    const auto at = out.find(mark);
    if (at == std::string_view::npos) return std::nullopt;
    out.remove_prefix(at + mark.size());
    RcloneVersion v;
    int* parts[] = {&v.major, &v.minor, &v.patch};
    for (std::size_t i = 0; i < 3; ++i) {
        const auto [end, err] = std::from_chars(out.data(), out.data() + out.size(), *parts[i]);
        if (err != std::errc{} || end == out.data()) return std::nullopt;
        out.remove_prefix(static_cast<std::size_t>(end - out.data()));
        if (i == 2) break;
        if (!out.starts_with('.')) return i == 1 ? std::optional(v) : std::nullopt; // "v1.70": no patch
        out.remove_prefix(1);
    }
    return v;
}

std::expected<void, std::string> check_rclone(const std::string& rclone, const std::filesystem::path& exe_dir,
                                              bool configured) {
#ifdef _WIN32
    const auto bundled = exe_dir / "rclone.exe";
#else
    const auto bundled = exe_dir / "rclone";
#endif
    if (!exe_dir.empty() && rclone == util::path_to_utf8(bundled)) return {}; // the one grab ships
    static std::mutex m;
    static std::map<std::string, std::string> checked; // program -> why it can't be used ("" fine)
    {
        std::lock_guard lock(m);
        if (const auto it = checked.find(rclone); it != checked.end()) {
            if (it->second.empty()) return {};
            return util::fail(it->second);
        }
    }
    proc::RunOptions quiet;
    quiet.detached = true;
    const auto ran = proc::run_capture(std::vector<std::string>{rclone, "version"}, quiet);
    std::string why;
    // One that can't run at all is reported by the call that needs it (ssh searches don't).
    if (ran && ran->exit_code == 0) {
        if (const auto v = parse_rclone_version(ran->out); v && *v < min_rclone) {
            why = std::format("{} is rclone {}.{}.{}, too old for grab, which needs {}.{} or newer (its folder downloads "
                              "use --sftp-connections). {}",
                              rclone, v->major, v->minor, v->patch, min_rclone.major, min_rclone.minor,
                              configured ? "Remove `rclone =` from grab.conf to use the rclone that comes with grab"
                                         : "The rclone.exe that comes with grab is missing next to grab.exe; "
                                           "reinstall grab or run `grab update`");
        }
    }
    std::lock_guard lock(m);
    checked.emplace(rclone, why);
    if (why.empty()) return {};
    return util::fail(why);
}

std::expected<Context, std::string> load_context(const std::filesystem::path& config_path,
                                                 const std::optional<std::string>& remote_name) {
    auto cfg = load_grab_config(config_path);
    if (!cfg) return util::fail(cfg.error());
    auto selected = cfg->select(remote_name);
    if (!selected) return util::fail(selected.error());
    RemoteSettings settings = **selected;

    // Move referenced remotes into grab's own rclone.conf the first time (idempotent).
    auto imported = migrate_rclone_remotes(*cfg);
    // grab's own rclone.conf keeps no secrets: they come from the encrypted store (secrets.hpp).
    std::string secrets_error;
    if (!cfg->rclone_config) {
        if (auto ready = secrets::prepare(grab_rclone_config_path()); !ready) secrets_error = ready.error();
    }

    const auto exe_dir = util::self_exe_path().parent_path();
    cfg->rclone = resolve_rclone_exe(*cfg, exe_dir);
    if (auto usable = check_rclone(cfg->rclone, exe_dir, !util::trim(cfg->rclone).empty()); !usable) {
        return util::fail(usable.error());
    }
    cfg->rclone_config = effective_rclone_config(*cfg); // always passed as --config
    auto remote = load_rclone_remote(*cfg->rclone_config, settings.rclone_remote);
    if (!remote) {
        std::string msg = remote.error();
        if (!imported.error.empty()) msg += std::format(" (import from rclone.conf failed: {})", imported.error);
        msg += std::format(". Add the server with `grab server add`.");
        return util::fail(msg);
    }

    if (!secrets_error.empty() && !remote->key_use_agent && !remote->key_file) {
        return util::failf("{}. Enter the password of {} again: Settings → Edit in grab's window, or `grab server "
                           "remove {}` and `grab server add {}`",
                           secrets_error, settings.name, settings.name, settings.name);
    }
    const FindMethod method = resolve_find_method(settings, *remote);
    const Engine engine = effective_engine(settings);
    const GrabMethod grab_method = resolve_grab_method(settings, *remote);
    return Context{.config = std::move(*cfg),
                   .settings = std::move(settings),
                   .remote = std::move(*remote),
                   .method = method,
                   .imported = std::move(imported.imported),
                   .engine = engine,
                   .grab_method = grab_method};
}

std::expected<ssh::Target, std::string> ssh_target(const Context& ctx, std::span<const std::string> extra) {
    const auto& conf = ctx.config.rclone_config ? *ctx.config.rclone_config : grab_rclone_config_path();
    secrets::Store store;
    if (auto stored = secrets::load(secrets::path_beside(conf)); stored) store = std::move(*stored);
    else return util::fail(stored.error());
    // An rclone.conf that isn't grab's own keeps its secrets inside, as rclone writes them.
    if (auto doc = ini::parse_file(conf); doc) {
        if (const auto* section = doc->find(ctx.remote.name)) {
            for (const auto& [key, value] : section->values) {
                if (secrets::is_secret_key(key) && !util::trim(value).empty()) store[ctx.remote.name][key] = value;
            }
        }
    }
    auto target = ssh::target_for(ctx.remote, store, grab_known_hosts_path());
    if (!target) return target;
    // --bind, --contimeout and --timeout reach grab's connections as they reached rclone's (G189).
    if (auto flags = ssh::apply_flags(*target, rclone_flags::connection(ctx.settings.common_flags)); !flags) {
        return util::failf("[{}] common_flags: {}", ctx.settings.name, flags.error());
    }
    if (auto flags = ssh::apply_flags(*target, extra); !flags) return util::failf("the flags after --: {}", flags.error());
    return target;
}

std::vector<std::string> unused_flags(const Context& ctx) {
    ssh::Target ignored;
    auto unused = ssh::apply_flags(ignored, rclone_flags::connection(ctx.settings.common_flags));
    return unused ? std::move(*unused) : std::vector<std::string>{};
}

namespace {

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// The last non-empty line of a child's stderr, without a trailing period (the caller's
// message continues after it).
std::string last_line(std::string_view text) {
    text = util::trim(text);
    const auto nl = text.rfind('\n');
    auto line = util::trim(nl == std::string_view::npos ? text : text.substr(nl + 1));
    while (line.ends_with('.')) line.remove_suffix(1);
    return std::string(line);
}

std::expected<std::vector<RemoteEntry>, std::string>
lookup_via_ssh(const Context& ctx, const SearchRequest& req, const std::vector<std::string>& roots,
               int max_depth, const proc::RunOptions& run, const CommandHook& on_command) {
    FindRequest find;
    find.mode = req.mode;
    find.target = req.target;
    find.exact = req.exact;
    find.roots = roots;
    find.max_depth = max_depth;
    find.skip_hidden = ctx.settings.skip_hidden;
    auto out = run_over_ssh(ctx, build_find_command(find), req.batch_ssh, run, on_command);
    if (!out) return util::fail(out.error());
    return parse_find_output(*out, req.mode == Mode::file);
}

// The entries of each kind in `kinds`, from one walk per root whatever their number.
std::expected<std::vector<std::vector<RemoteEntry>>, std::string>
lookup_via_rclone(const Context& ctx, const SearchRequest& req, std::span<const Mode> kinds,
                  const std::vector<std::string>& roots, int max_depth, const proc::RunOptions& run,
                  const CommandHook& on_command) {
    // A path target is checked with a single listing of its parent, whatever the roots.
    const std::vector<std::string> bases =
        is_path_target(req.target) ? std::vector<std::string>{""} : roots;

    std::vector<std::vector<RemoteEntry>> found(kinds.size());
    for (const auto& root : bases) {
        ListRequest list;
        list.mode = kinds.front();
        list.all_entries = kinds.size() > 1;
        list.target = req.target;
        list.exact = req.exact;
        list.root = root;
        list.max_depth = max_depth;
        list.skip_hidden = ctx.settings.skip_hidden;
        list.rclone_exe = ctx.config.rclone;
        list.rclone_config = ctx.config.rclone_config;
        list.rclone_remote = ctx.settings.rclone_remote;
        list.flags = rclone_flags::connection(ctx.settings.common_flags); // a --bind reaches searches too (G189)
        list.flags.insert(list.flags.end(), req.walk_flags.begin(), req.walk_flags.end());

        auto out = run_listing(build_lsf_argv(list), run, on_command);
        if (!out) return util::fail(out.error());
        for (std::size_t k = 0; k < kinds.size(); ++k) {
            list.mode = kinds[k];
            auto entries = parse_lsf_output(list, *out);
            found[k].insert(found[k].end(), std::make_move_iterator(entries.begin()),
                            std::make_move_iterator(entries.end()));
        }
    }
    return found;
}

// A path target on grab's engine: one STAT, which follows links as find -L and the walk do.
// nullopt when nothing is there (or a socket, a device: nothing to download).
struct Stated {
    RemoteEntry entry;
    bool dir = false;
};
std::expected<std::optional<Stated>, std::string> stat_target(ssh::Pool& pool, std::string_view target,
                                                              const std::stop_token& stop, const CommandHook& on_command) {
    // As a listing of its parent names it (parse_lsf_output).
    const std::string path = join_remote(parent_of(target), remote_basename(target));
    if (!has_name(path)) return std::nullopt; // ".", "..": never an entry of a listing (review G15)
    if (on_command) on_command({"sftp", "stat", path});
    std::expected<sftp::Attrs, sftp::Status> attrs;
    // A STAT is safe to repeat: a reused connection the server closed while it sat idle is dropped
    // and the next one tried.
    for (bool done = false; !done;) {
        std::optional<ssh::Pool::Lease> lease;
        try {
            lease = pool.acquire(stop, ssh::interactive_retry);
        } catch (const ssh::Error& e) {
            return util::fail(e.what());
        }
        if (!lease) return util::fail("search cancelled");
        try {
            attrs = sftp::stat((*lease)->sftp(), rclone_enc::raw_path(path));
            done = true;
        } catch (const sftp::Broken& e) {
            lease->broken();
            if (!lease->reused()) return util::failf("cannot look at {}: {}", path, e.what());
        }
    }
    if (!attrs) {
        if (attrs.error().code == sftp::Code::no_such_file) return std::nullopt;
        return util::failf("cannot look at {}: {}", path, sftp::describe(attrs.error()));
    }
    if (!attrs->is_dir() && !attrs->is_file()) return std::nullopt;
    const bool dir = attrs->is_dir();
    return Stated{.entry = RemoteEntry{path, dir ? std::nullopt : attrs->size,
                                       attrs->mtime ? std::optional<std::int64_t>(*attrs->mtime) : std::nullopt},
                  .dir = dir};
}

// What a lookup on grab's engine found: the entries of each kind asked for, and the notes.
struct GrabFound {
    std::vector<std::vector<RemoteEntry>> kinds;
    std::vector<std::string> notes;
};

// The entries of each kind in `kinds` through grab's own SSH client: a path target with one STAT;
// a name search with one find (both kinds at once) or one walk, whatever their number.
std::expected<GrabFound, std::string> lookup_via_grab(const Context& ctx, const SearchRequest& req,
                                                      std::span<const Mode> kinds, const std::vector<std::string>& roots,
                                                      int max_depth, const std::stop_token& stop,
                                                      const CommandHook& on_command) {
    auto pool = grab_pool(ctx, req.grab);
    if (!pool) return util::fail(pool.error());
    GrabFound found;
    found.kinds.resize(kinds.size());
    auto kind_of = [&](bool dir) -> std::vector<RemoteEntry>* {
        for (std::size_t k = 0; k < kinds.size(); ++k) {
            if ((kinds[k] == Mode::folder) == dir) return &found.kinds[k];
        }
        return nullptr;
    };
    if (is_path_target(req.target)) {
        auto stated = stat_target(**pool, req.target, stop, on_command);
        if (!stated) return util::fail(stated.error());
        if (*stated) {
            if (auto* into = kind_of((*stated)->dir)) into->push_back(std::move((*stated)->entry));
        }
        return found;
    }

    auto way = grab_lookup(ctx, **pool, req.grab, stop);
    if (!way) return util::fail(way.error());
    const Query query = make_query(req.target, req.exact);
    const bool folders = std::ranges::find(kinds, Mode::folder) != kinds.end();
    const bool files = std::ranges::find(kinds, Mode::file) != kinds.end();
    if (way->find) {
        const ssh::FindSpec spec{.roots = roots,
                                 .max_depth = max_depth,
                                 .skip_hidden = ctx.settings.skip_hidden,
                                 .query = query,
                                 .folders = folders,
                                 .files = files};
        ssh::FindOptions options;
        options.stop = stop;
        options.progress = req.grab.progress;
        options.retry_refused = ssh::interactive_retry;
        auto got = grab_find(ctx, **pool, req.grab, *way, spec, options, on_command);
        if (!got) return util::fail(got.error());
        if (*got) {
            for (auto& e : (*got)->entries) {
                if (auto* into = kind_of(e.dir)) into->push_back(RemoteEntry{std::move(e.path), e.size, e.modified});
            }
            found.notes = std::move((*got)->notes);
            return found;
        }
        // Asked again, the check says walk.
    }

    const int seat = req.grab.walk_seat ? req.grab.walk_seat() : 4;
    if (stop.stop_requested()) return util::fail("search cancelled");
    ssh::WalkOptions options{.connections = std::max(1, seat),
                             .folders = folders,
                             .max_depth = max_depth,
                             .skip_hidden = ctx.settings.skip_hidden && !wants_hidden(query)};
    options.progress = req.grab.progress;
    options.retry_refused = ssh::interactive_retry;
    if (on_command) {
        std::vector<std::string> argv{"sftp", "walk"};
        for (const auto& r : roots) argv.push_back(r.empty() ? std::string(".") : r);
        argv.insert(argv.end(), {"--max-depth", std::to_string(max_depth)});
        on_command(argv);
    }
    auto walked = ssh::walk_roots(**pool, roots, options, stop);
    if (stop.stop_requested()) return util::fail("search cancelled");
    if (!walked) return util::fail(walked.error());
    // The walk left dot-names out already, as a listing's filter would.
    std::vector<HitFilter> filters;
    filters.reserve(kinds.size());
    for (const Mode k : kinds) filters.emplace_back(k, req.target, req.exact, false);
    for (auto& e : walked->entries) {
        for (std::size_t k = 0; k < kinds.size(); ++k) {
            if (!filters[k].keeps(e.path, e.dir)) continue;
            found.kinds[k].push_back(RemoteEntry{std::move(e.path), e.dir ? std::nullopt : std::optional(e.size), e.modified});
            break;
        }
    }
    found.notes = std::move(walked->unreadable);
    return found;
}

} // namespace

std::expected<std::shared_ptr<ssh::Pool>, std::string> grab_pool(const Context& ctx, const GrabSide& side) {
    if (side.pool) return side.pool;
    auto target = ssh_target(ctx);
    if (!target) return util::fail(target.error());
    return std::make_shared<ssh::Pool>([t = std::move(*target)] { return ssh::open_session(t); },
                                       ssh::Pools::download_options(4));
}

std::expected<ssh::Lookup, std::string> grab_lookup(const Context& ctx, ssh::Pool& pool, const GrabSide& side,
                                                    std::stop_token stop) {
    auto way = ssh::choose_lookup(pool, ctx.grab_method, ctx.settings.name, stop, ssh::interactive_retry, side.memory);
    if (stop.stop_requested()) return util::fail("search cancelled");
    if (!way) return util::fail(way.error());
    if (side.on_lookup) side.on_lookup(*way);
    return way;
}

std::expected<std::optional<ssh::Found>, std::string> grab_find(const Context& ctx, ssh::Pool& pool,
                                                                const GrabSide& side, ssh::Lookup& way,
                                                                const ssh::FindSpec& spec,
                                                                const ssh::FindOptions& options,
                                                                const CommandHook& on_command) {
    for (bool again = false;; again = true) {
        if (on_command) on_command({"exec", ssh::find_command(spec)});
        auto found = ssh::run_find(pool, spec, options);
        if (options.stop.stop_requested()) return util::fail("search cancelled");
        if (found) return std::optional(std::move(*found));
        // The check's answer was asked just now (for this search, an earlier one in the program
        // or another caller's), or asked again already: asking once more would only repeat it.
        const bool fresh =
            way.checked > 0 && unix_now() - way.checked < std::chrono::seconds(ssh::find_check_fresh).count();
        if (!found.error().about_find || way.asked || fresh || again) {
            return util::fail(std::move(found.error().message));
        }
        // A saved answer (or one from earlier in the program) says find works, and find says it
        // doesn't (any more): asked again now, so this search goes the way that works.
        auto asked = ssh::recheck_lookup(pool, ctx.grab_method, ctx.settings.name, options.stop,
                                         ssh::interactive_retry, side.memory);
        if (options.stop.stop_requested()) return util::fail("search cancelled");
        if (!asked) return util::fail(asked.error());
        way = std::move(*asked);
        way.redone = std::move(found.error().message);
        if (side.on_lookup) side.on_lookup(way);
        if (!way.find) return std::nullopt;
    }
}

ssh::FindMemory find_memory(const std::filesystem::path& limits_file, const ssh::Target& target,
                            std::function<void(const std::expected<void, std::string>&)> report,
                            std::map<std::string, int> before) {
    const std::string login = std::format("{}@{}:{}", target.user, target.host, target.port);
    ssh::FindMemory memory;
    memory.load = [limits_file, login]() -> std::optional<ssh::FindCheck> {
        auto kept = load_find_check(limits_file, login);
        if (!kept) return std::nullopt;
        // Trusted for a week; one from the future (a clock set back) isn't.
        const auto age = std::chrono::seconds(unix_now() - kept->checked);
        if (age < std::chrono::seconds(0) || age >= ssh::find_check_trust) return std::nullopt;
        return ssh::FindCheck{.works = kept->works,
                              .answer = std::move(kept->answer),
                              .lasting = true,
                              .checked = kept->checked,
                              .saved = true};
    };
    memory.save = [limits_file, login, report = std::move(report),
                   before = std::move(before)](const std::optional<ssh::FindCheck>& check) {
        std::optional<SavedFindCheck> keep;
        if (check) keep = SavedFindCheck{.works = check->works, .answer = check->answer, .checked = check->checked};
        // Answers past their week go as well.
        const auto saved = save_find_check(limits_file, login, keep,
                                           unix_now() - std::chrono::seconds(ssh::find_check_trust).count(), before);
        if (report) report(saved);
    };
    return memory;
}

std::string lookup_text(const Context& ctx, const ssh::Lookup& way) {
    // An answer an earlier run saved, with its date.
    const std::string on = way.saved && way.checked > 0 ? " on " + util::local_date(way.checked) : std::string();
    std::string text;
    if (way.find) {
        text = std::format("with find ({}{})", way.answer, way.saved ? ", checked" + on : std::string());
    } else if (!way.answer.empty()) {
        text = std::format("by listing folders (its answer to grab's find check{}: {})", on, way.answer);
    } else if (ctx.settings.find == FindMethod::auto_detect && budget::is_storage_box(ctx.remote.host)) {
        // The check wasn't asked.
        text = "by listing folders (a Storage Box has no find, so grab doesn't ask)";
    } else {
        text = std::format("by listing folders, as [{}]'s find setting says", ctx.settings.name);
    }
    if (!way.redone.empty()) text += "; asked again after: " + way.redone;
    return text;
}

std::vector<std::string> search_roots(const Context& ctx) {
    auto roots = ctx.settings.search_roots;
    if (roots.empty()) roots.emplace_back(); // the login home
    return roots;
}

std::expected<std::string, std::string> run_over_ssh(const Context& ctx, const std::string& command, bool batch_ssh,
                                                     const proc::RunOptions& run, const CommandHook& on_command) {
    std::vector<std::string> ssh_options = ctx.settings.ssh_options;
    if (batch_ssh) ssh_options.insert(ssh_options.end(), {"-o", "BatchMode=yes"});
    const auto argv = build_ssh_argv(ctx.config.ssh, ctx.remote, ssh_options, command);
    if (on_command) on_command(argv);

    auto res = proc::run_capture(argv, run);
    if (!res) return util::fail(res.error());
    if (res->exit_code == proc::exit_stopped && run.stop.stop_requested()) {
        return util::fail("search cancelled");
    }
    if (res->exit_code == 255) {
        const std::string detail = run.detached ? last_line(res->err) : std::string{};
        std::string msg = std::format("ssh to {}@{}:{} failed (exit 255)", ctx.remote.user,
                                      ctx.remote.host, ctx.remote.port);
        if (!detail.empty()) msg += ": " + detail;
        else if (!run.detached) msg += "; see the message above";
        if (batch_ssh) {
            msg += ". grab cannot answer a passphrase prompt here; load the key into ssh-agent "
                   "(ssh-add <key file>)";
        }
        msg += std::format(". If this host has no shell (e.g. a storage box), set `find = rclone` "
                           "in [{}]",
                           ctx.settings.name);
        return util::fail(msg);
    }
    return std::move(res->out);
}

std::expected<std::string, std::string> run_listing(std::span<const std::string> argv, const proc::RunOptions& run,
                                                    const CommandHook& on_command) {
    if (on_command) on_command(std::vector<std::string>(argv.begin(), argv.end()));
    auto res = proc::run_capture(argv, run);
    if (!res) return util::fail(res.error());
    if (res->exit_code == proc::exit_stopped && run.stop.stop_requested()) {
        return util::fail("search cancelled");
    }
    if (res->exit_code != 0) {
        const std::string detail = run.detached ? last_line(res->err) : std::string{};
        return util::failf("rclone lsf of '{}' failed (exit {}){}", argv[2], res->exit_code,
                           detail.empty() ? (run.detached ? "" : "; see the message above") : ": " + detail);
    }
    return std::move(res->out);
}

std::expected<SearchResult, std::string> search(const Context& ctx, const SearchRequest& req,
                                                const proc::RunOptions& run,
                                                const CommandHook& on_command) {
    const auto roots = search_roots(ctx);
    const int max_depth = req.depth.value_or(ctx.settings.max_depth);
    if (ctx.engine == Engine::grab) {
        const Mode kind = req.mode;
        auto found = lookup_via_grab(ctx, req, std::span(&kind, 1), roots, max_depth, run.stop, on_command);
        if (!found) return util::fail(found.error());
        auto result = make_result(ctx, req, std::move(found->kinds.front()));
        result.notes = std::move(found->notes);
        return result;
    }
    if (ctx.method == FindMethod::ssh) {
        auto entries = lookup_via_ssh(ctx, req, roots, max_depth, run, on_command);
        if (!entries) return util::fail(entries.error());
        return make_result(ctx, req, std::move(*entries));
    }
    const Mode kind = req.mode;
    auto entries = lookup_via_rclone(ctx, req, std::span(&kind, 1), roots, max_depth, run, on_command);
    if (!entries) return util::fail(entries.error());
    return make_result(ctx, req, std::move(entries->front()));
}

std::expected<std::pair<SearchResult, SearchResult>, std::string>
search_both(const Context& ctx, const SearchRequest& req, const proc::RunOptions& run, const CommandHook& on_command) {
    auto folders = req;
    folders.mode = Mode::folder;
    auto files = req;
    files.mode = Mode::file;
    if (ctx.engine == Engine::grab) {
        constexpr std::array kinds{Mode::folder, Mode::file};
        auto found = lookup_via_grab(ctx, req, kinds, search_roots(ctx), req.depth.value_or(ctx.settings.max_depth),
                                     run.stop, on_command);
        if (!found) return util::fail(found.error());
        auto f = make_result(ctx, folders, std::move(found->kinds[0]));
        auto g = make_result(ctx, files, std::move(found->kinds[1]));
        f.notes = found->notes; // one lookup's, the same for both kinds
        g.notes = std::move(found->notes);
        return std::pair{std::move(f), std::move(g)};
    }
    if (ctx.method == FindMethod::ssh) {
        auto f = search(ctx, folders, run, on_command);
        if (!f) return util::fail(f.error());
        auto g = search(ctx, files, run, on_command);
        if (!g) return util::fail(g.error());
        return std::pair{std::move(*f), std::move(*g)};
    }
    constexpr std::array kinds{Mode::folder, Mode::file};
    auto entries = lookup_via_rclone(ctx, req, kinds, search_roots(ctx), req.depth.value_or(ctx.settings.max_depth),
                                     run, on_command);
    if (!entries) return util::fail(entries.error());
    return std::pair{make_result(ctx, folders, std::move((*entries)[0])), make_result(ctx, files, std::move((*entries)[1]))};
}

SearchResult make_result(const Context& ctx, const SearchRequest& req, std::vector<RemoteEntry> entries) {
    SearchResult result;
    result.max_depth = req.depth.value_or(ctx.settings.max_depth);
    result.roots = search_roots(ctx);

    // Rank by path, then reattach sizes and dates.
    struct Known {
        std::optional<std::uint64_t> size;
        std::optional<std::int64_t> modified;
    };
    std::unordered_map<std::string, Known> known;
    std::vector<std::string> paths;
    paths.reserve(entries.size());
    for (auto& e : entries) {
        if (known.emplace(e.path, Known{e.size, e.modified}).second) paths.push_back(std::move(e.path));
    }
    rank_matches(paths, make_query(req.target, req.exact));
    result.hits.reserve(paths.size());
    for (auto& p : paths) {
        const auto k = known[p];
        std::string name = remote_basename(p);
        result.hits.push_back(Hit{std::move(p), std::move(name), k.size, false, k.modified});
    }
    return result;
}

std::expected<std::vector<std::string>, std::string>
download_argv(const Context& ctx, Mode mode, const std::string& remote_path, const std::filesystem::path& dest_dir,
              std::span<const std::string> extra, std::optional<std::filesystem::path> local_name) {
    RcloneRequest rq;
    rq.mode = mode;
    rq.rclone_exe = ctx.config.rclone;
    rq.rclone_config = ctx.config.rclone_config;
    rq.rclone_remote = ctx.settings.rclone_remote;
    rq.remote_path = remote_path;
    rq.dest_dir = dest_dir;
    rq.common_flags = ctx.settings.common_flags;
    rq.mode_flags = mode == Mode::folder ? ctx.settings.folder_flags : ctx.settings.file_flags;
    rq.extra = extra;
    rq.local_name = std::move(local_name);
    return build_rclone_argv(rq);
}

std::optional<Progress> parse_stats_line(std::string_view line) {
    if (line.find("\"stats\"") == std::string_view::npos) return std::nullopt; // cheap pre-check
    auto doc = json::parse(line);
    if (!doc) return std::nullopt;
    const json::Value* stats = doc->find("stats");
    if (stats == nullptr || stats->kind != json::Value::Kind::object) return std::nullopt;

    auto number = [](const json::Value* v) -> std::optional<double> {
        if (v == nullptr || v->kind != json::Value::Kind::number) return std::nullopt;
        return v->number;
    };
    auto to_bytes = [](double d) { return d > 0 ? static_cast<std::uint64_t>(d) : std::uint64_t{0}; };

    Progress p;
    p.bytes = to_bytes(number(stats->find("bytes")).value_or(0));
    p.total = to_bytes(number(stats->find("totalBytes")).value_or(0));
    p.speed = number(stats->find("speed")).value_or(0);
    p.eta = number(stats->find("eta"));

    // Early in a transfer the overall speed is still 0 while each running transfer already
    // reports its own; the ETA can be null. Fill both in from what is known.
    double transferring_speed = 0;
    if (const auto* list = stats->find("transferring");
        list != nullptr && list->kind == json::Value::Kind::array) {
        for (const auto& t : list->items) {
            const double speed = number(t.find("speed")).value_or(0);
            transferring_speed += speed;
            auto name = t.string_of("name");
            if (!name) continue;
            if (p.current.empty()) p.current = *name;
            p.transferring.push_back(Transfer{*name, to_bytes(number(t.find("bytes")).value_or(0)),
                                              to_bytes(number(t.find("size")).value_or(0)), speed});
        }
    }
    if (p.speed <= 0) p.speed = transferring_speed;
    if (!p.eta && p.speed > 0 && p.total > p.bytes) {
        p.eta = static_cast<double>(p.total - p.bytes) / p.speed;
    }
    return p;
}

std::optional<std::string> parse_error_line(std::string_view line) {
    if (line.find("\"level\"") == std::string_view::npos) return std::nullopt;
    auto doc = json::parse(line);
    if (!doc || doc->string_of("level") != "error") return std::nullopt;
    return doc->string_of("msg");
}

std::expected<DownloadResult, std::string>
download(const Context& ctx, Mode mode, const std::string& remote_path,
         const std::filesystem::path& dest_dir, const std::function<void(const Progress&)>& on_progress,
         const proc::RunOptions& run, std::span<const std::string> extra,
         const CommandHook& on_command, const std::function<void(const std::string&)>& on_copied,
         const std::optional<std::filesystem::path>& local_name) {
    // grab's own batch: only the connection flags of the configured ones (rclone_flags.hpp);
    // `extra` is grab's (--files-from-raw, --transfers...).
    const auto common = rclone_flags::connection(ctx.settings.common_flags);
    const auto mode_flags =
        rclone_flags::connection(mode == Mode::folder ? ctx.settings.folder_flags : ctx.settings.file_flags);
    std::vector<std::string> tail(extra.begin(), extra.end());
    // Appended last so they win over anything configured.
    tail.insert(tail.end(), {"--use-json-log", "--stats", "500ms", "--stats-log-level", "NOTICE"});

    RcloneRequest rq;
    rq.mode = mode;
    rq.rclone_exe = ctx.config.rclone;
    rq.rclone_config = ctx.config.rclone_config;
    rq.rclone_remote = ctx.settings.rclone_remote;
    rq.remote_path = remote_path;
    rq.dest_dir = dest_dir;
    rq.local_name = local_name;
    rq.common_flags = common;
    rq.mode_flags = mode_flags;
    rq.extra = tail;
    const auto built = build_rclone_argv(rq);
    if (!built) return util::fail(built.error());
    const auto& argv = *built;
    if (on_command) on_command(argv);

    DownloadResult result;
    auto code = proc::run_streaming(
        argv,
        [&](std::string_view line) {
            if (auto p = parse_stats_line(line)) {
                if (on_progress) on_progress(*p);
            } else if (auto msg = parse_error_line(line)) {
                result.last_error = std::move(*msg);
            } else if (on_copied) {
                if (auto name = folder::parse_copied_line(line)) on_copied(*name);
            }
        },
        run);
    if (!code) return util::fail(code.error());
    result.exit_code = *code;
    return result;
}

std::vector<std::filesystem::path> rclone_temp_files(std::span<const std::filesystem::path> files) {
    namespace fs = std::filesystem;
    using Name = fs::path::string_type;
    // The files by folder: their names, and the long ones (cut short in the temp name).
    struct Folder {
        std::unordered_set<Name> names;
        std::vector<Name> long_names;
    };
    std::map<fs::path, Folder> folders;
    for (const auto& f : files) {
        auto& folder = folders[f.parent_path()];
        folder.names.insert(f.filename().native());
        if (util::path_to_utf8(f.filename()).size() > 100) folder.long_names.push_back(f.filename().native());
    }
    const Name suffix = fs::path(".partial").native();
    std::vector<fs::path> found;
    for (const auto& [dir, folder] : folders) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            const Name entry = e.path().filename().native();
            // <name> "." 8 hex ".partial"
            if (entry.size() < suffix.size() + 10 || !entry.ends_with(suffix)) continue;
            const auto dot = entry.size() - suffix.size() - 9;
            if (entry[dot] != '.') continue;
            const bool hex = std::all_of(entry.begin() + static_cast<std::ptrdiff_t>(dot) + 1,
                                         entry.end() - static_cast<std::ptrdiff_t>(suffix.size()), [](auto c) {
                                             return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
                                         });
            if (!hex) continue;
            const Name name = entry.substr(0, dot);
            const bool ours = folder.names.contains(name) ||
                              std::ranges::any_of(folder.long_names, [&](const Name& n) {
                                  return !name.empty() && n.size() > name.size() && n.starts_with(name);
                              });
            if (ours && e.is_regular_file(ec)) found.push_back(e.path());
        }
    }
    return found;
}

std::size_t remove_rclone_temp_files(std::span<const std::filesystem::path> files,
                                     std::span<const std::filesystem::path> before,
                                     const std::function<bool(const std::filesystem::path&)>& keep) {
    const std::set<std::filesystem::path> there(before.begin(), before.end());
    std::size_t removed = 0;
    for (const auto& p : rclone_temp_files(files)) {
        if (there.contains(p) || (keep && keep(p))) continue;
        std::error_code ec;
        if (std::filesystem::remove(p, ec)) ++removed;
    }
    return removed;
}

} // namespace grab::engine
