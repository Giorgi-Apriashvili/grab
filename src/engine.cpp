#include "engine.hpp"

#include "conn_budget.hpp"
#include "gui_state.hpp"
#include "listing.hpp"
#include "match.hpp"
#include "passwords.hpp"
#include "rclone_encoding.hpp"
#include "rclone_flags.hpp"
#include "rclone_import.hpp"
#include "remote.hpp"
#include "ini.hpp"
#include "sftp.hpp"
#include "ssh_find.hpp"
#include "ssh_target.hpp"
#include "ssh_walk.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>

namespace grab::engine {

Context make_context(GrabConfig config, RemoteSettings settings, server_conf::Connection connection,
                     const std::filesystem::path& grab_conf) {
    Context ctx;
    ctx.remote = server_conf::to_remote(settings.rclone_remote, connection, grab_known_hosts_path());
    ctx.grab_method = resolve_grab_method(settings, ctx.remote);
    ctx.config = std::move(config);
    ctx.settings = std::move(settings);
    ctx.connection = std::move(connection);
    ctx.passwords = passwords::path_in(state_dir(grab_conf));

    // The guard (phase 8 step 4): the store's rclone extras for this very login, as the import
    // kept them. A store that exists but can't be read may hold a proxy or an ssh command: the
    // guard then counts the server as routed rather than connect past one.
    std::vector<std::pair<std::string, std::string>> extras;
    const auto store = passwords::load(ctx.passwords);
    if (store) {
        const passwords::Binding login{.user = ctx.connection.user, .host = ctx.connection.host, .port = ctx.connection.port};
        extras = guard::store_extras(*store, ctx.settings.name, login);
    }
    auto sources = guard::sources(ctx.settings, ctx.connection, std::move(extras));
    ctx.guard = guard::evaluate(sources);
    if (!store) guard::add_unreadable_store(ctx.guard);
    for (auto& [key, value] : sources.extras) std::ranges::fill(value, '\0');
    return ctx;
}

std::expected<Context, std::string> load_context(const std::filesystem::path& config_path,
                                                 const std::optional<std::string>& remote_name, bool import) {
    // grab 0.19's servers move from rclone.conf into grab.conf once.
    std::optional<std::expected<rclone_import::Result, std::string>> applied;
    if (import) applied = rclone_import::apply(rclone_import::files_for(config_path));

    auto doc = ini::parse_file(config_path);
    if (!doc) return util::fail(doc.error());
    auto cfg = parse_grab_config(*doc);
    if (!cfg) return util::failf("{}: {}", util::path_to_utf8(config_path), cfg.error());
    auto selected = cfg->select(remote_name);
    if (!selected) return util::fail(selected.error());
    RemoteSettings settings = **selected;
    std::expected<std::optional<server_conf::Connection>, std::string> connection;
    if (const auto* section = doc->find(settings.name)) connection = server_conf::parse(*section);
    if (!connection) return util::failf("{}: {}", util::path_to_utf8(config_path), connection.error());
    if (!*connection) {
        // A section without details: what the import said about it, if it tried.
        std::string why;
        if (applied && !*applied) why = applied->error();
        if (applied && *applied) {
            for (const auto& note : (*applied)->notes) {
                if (note.server == settings.name && note.text.starts_with("not imported")) why = note.text;
            }
        }
        return util::failf("[{}] has no connection details (host, port, user, auth) in grab.conf{}. Add them with "
                           "`grab server edit {}` or Edit in grab's Settings",
                           settings.name, why.empty() ? std::string() : " (" + why + ")", settings.name);
    }

    Context ctx = make_context(std::move(*cfg), std::move(settings), std::move(**connection), config_path);
    if (applied && !*applied) ctx.import_notes.push_back(applied->error());
    if (applied && *applied) {
        for (const auto& server : (*applied)->imported) ctx.imported.push_back(server.name);
        for (const auto& note : (*applied)->notes) ctx.import_notes.push_back(std::format("[{}] {}", note.server, note.text));
    }
    return ctx;
}

std::expected<void, std::string> check_pinned(const Context& ctx) {
    const auto file = ctx.remote.known_hosts_file ? util::path_from_utf8(*ctx.remote.known_hosts_file)
                                                  : grab_known_hosts_path();
    auto text = util::read_file_if_exists(file);
    if (!text) return util::failf("grab's known_hosts can't be read: {}", text.error());
    if (server_conf::pinned(text->value_or(""), ctx.remote.host, ctx.remote.port)) return {};
    return util::failf("{}: host key not trusted: use Trust (Settings → Trust in grab's window, or `grab server trust "
                       "{}`), which shows the key's fingerprints to compare",
                       ctx.settings.name, ctx.settings.name);
}

namespace {

// grab's store of the context's server, when it has one that can be read. One that can't is an
// error for a server that can't log in without it, with what to do about it: a change through Edit
// sets the unreadable file aside (passwords::load_for_change) and starts a new one. (The guard
// refuses every server of a store that can't be read before this: guard::add_unreadable_store.)
std::expected<passwords::Store, std::string> server_secrets(const Context& ctx) {
    if (ctx.passwords.empty()) return passwords::Store{};
    auto store = passwords::load(ctx.passwords);
    if (store) return store;
    const std::string& server = ctx.settings.name;
    const std::string edit = std::format("with Edit (Settings → Edit in grab's window, or `grab server edit {}`)", server);
    switch (ctx.connection.auth) {
    case server_conf::Auth::password:
        return util::failf("{}. Enter the password of {} again {}, which sets the unreadable file aside", store.error(),
                           server, edit);
    case server_conf::Auth::stored_key:
        return util::failf("{}. {} logs in with a key kept there: choose a key file or ssh-agent for it {}, which sets "
                           "the unreadable file aside",
                           store.error(), server, edit);
    default: break;
    }
    return passwords::Store{};
}

// Why a --bind can't be used, nullopt when it can: one that isn't an address is refused before
// anything connects.
std::optional<std::string> bind_problem(const std::string& bind) {
    if (bind.starts_with('-')) return "the address can't start with -";
    return server_conf::check_host(bind);
}

} // namespace

std::vector<guard::Reason> grab_blockers(const Context& ctx, std::span<const std::string> run_flags) {
    if (run_flags.empty()) return guard::reasons(ctx.guard);
    auto settings = ctx.guard;
    guard::add_run_flags(settings, run_flags);
    return guard::reasons(settings);
}

std::expected<ConnectSettings, std::string> connect_settings(const RemoteSettings& settings,
                                                             std::span<const std::string> run_flags) {
    const std::string& name = settings.name;
    ConnectSettings out;
    if (settings.bind) {
        out.bind = settings.bind;
        out.bind_from = std::format("bind in [{}]", name);
    }
    out.connect_timeout = settings.connect_timeout;
    out.io_timeout = settings.io_timeout;
    // Legacy: common_flags' --bind, --contimeout and --timeout, read as the keys left blank (G189).
    auto legacy = ssh::read_flags(rclone_flags::connection(settings.common_flags));
    if (!legacy) return util::failf("[{}] common_flags: {}", name, legacy.error());
    // A blank --bind ("--bind=") is no bind, as rclone read it.
    const auto blank = [](const std::optional<std::string>& bind) { return bind && util::trim(*bind).empty(); };
    if (!out.bind && legacy->bind && !blank(legacy->bind)) {
        if (auto bad = bind_problem(*legacy->bind)) return util::failf("[{}] common_flags: --bind {}: {}", name, *legacy->bind, *bad);
        out.bind = legacy->bind;
        out.bind_from = "common_flags' --bind";
        out.legacy.emplace_back("bind");
    }
    if (!out.connect_timeout && legacy->connect_timeout) {
        out.connect_timeout = legacy->connect_timeout;
        out.legacy.emplace_back("connect_timeout");
    }
    if (!out.io_timeout && legacy->timeout) {
        out.io_timeout = legacy->timeout;
        out.io_from = std::format("[{}] common_flags: --timeout {}", name, legacy->timeout_text);
        out.legacy.emplace_back("io_timeout");
    }
    // The command's, which win.
    auto given = ssh::read_flags(run_flags);
    if (!given) return util::failf("the flags after --: {}", given.error());
    if (blank(given->bind)) {
        // Clears the server's bind for this run.
        out.bind.reset();
        out.bind_from.clear();
    } else if (given->bind) {
        if (auto bad = bind_problem(*given->bind)) return util::failf("the flags after --: --bind {}: {}", *given->bind, *bad);
        out.bind = given->bind;
        out.bind_from = "--bind";
    }
    if (given->connect_timeout) out.connect_timeout = given->connect_timeout;
    if (given->timeout) {
        out.io_timeout = given->timeout;
        out.io_from = std::format("the flags after --: --timeout {}", given->timeout_text);
    }
    return out;
}

std::expected<void, std::string> check_run_flags(const Context& ctx, std::span<const std::string> extra) {
    const std::string& name = ctx.settings.name;
    if (const auto bad = rclone_flags::first_refused(extra)) {
        // A certificate or an ssh command isn't in grab's own client's settings at all.
        const bool unusable = bad->in_grab == "rclone.pubkey_file" || bad->in_grab == "ssh";
        return util::failf("{} after -- would replace the server's {}, which {}; {}", bad->flag, bad->what,
                           unusable ? "grab's own client can't use" : "grab's own client takes from its settings",
                           rclone_flags::change_instead(*bad, name));
    }
    // The 2026-10-21 rule: refused, never ignored.
    if (auto unused = rclone_flags::first_unused_by_grab(extra, name)) return util::fail(*unused);
    // The values of the connection flags, before anything connects or DEST is made.
    const auto given = ssh::read_flags(extra);
    if (!given) return util::failf("the flags after --: {}", given.error());
    if (given->bind && !given->bind->empty()) {
        if (auto bad = bind_problem(*given->bind)) return util::failf("the flags after --: --bind {}: {}", *given->bind, *bad);
    }
    // No limit would leave a stalled READ waiting for ever (ssh_target).
    if (given->timeout && (*given->timeout < min_io_timeout || *given->timeout > max_io_timeout)) {
        return util::failf("the flags after --: --timeout {}: grab's own client takes a time from 1s to 24h there (with "
                           "none, a stalled download couldn't be cancelled)",
                           given->timeout_text);
    }
    return {};
}

std::expected<ssh::Target, std::string> ssh_target(const Context& ctx, std::span<const std::string> extra) {
    // A route, another server, a certificate...: grab would connect anyway, past them. First, as
    // Trust is refused for such a server too (phase 9 step 6).
    if (const auto blocked = grab_blockers(ctx, extra); !blocked.empty()) {
        return util::fail(guard::text(ctx.settings.name, blocked));
    }
    if (auto pinned = check_pinned(ctx); !pinned) return util::fail(pinned.error());
    auto store = server_secrets(ctx);
    if (!store) return util::fail(store.error());
    const auto known_hosts = ctx.remote.known_hosts_file ? util::path_from_utf8(*ctx.remote.known_hosts_file)
                                                         : grab_known_hosts_path();
    auto target = ssh::target_for(ctx.settings.name, ctx.connection, *store, known_hosts);
    if (!target) return target;
    auto reach = connect_settings(ctx.settings, extra);
    if (!reach) return util::fail(reach.error());
    if (reach->bind) {
        target->bind = *reach->bind;
        target->bind_from = reach->bind_from;
    }
    if (reach->connect_timeout) target->connect_timeout = *reach->connect_timeout;
    if (reach->io_timeout) {
        // No limit, or one past what select and libssh2 hold, would leave a stalled READ waiting
        // for ever: sftp::Client::pump doesn't look at the stop token, so Cancel couldn't end it.
        if (*reach->io_timeout < min_io_timeout || *reach->io_timeout > max_io_timeout) {
            return util::failf("{}: grab's own client takes a time from 1s to 24h there (with none, a stalled download "
                               "couldn't be cancelled)",
                               reach->io_from);
        }
        target->timeout = *reach->io_timeout;
    }
    return target;
}

namespace {

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// A path target: one STAT, which follows links as find -L and the walk do. nullopt when nothing
// is there (or a socket, a device: nothing to download).
struct Stated {
    RemoteEntry entry;
    bool dir = false;
};
std::expected<std::optional<Stated>, std::string> stat_target(ssh::Pool& pool, std::string_view target,
                                                              const std::stop_token& stop, const CommandHook& on_command) {
    // As a search names it: its parent joined with its name.
    const std::string path = join_remote(parent_of(target), remote_basename(target));
    if (!has_name(path)) return std::nullopt; // ".", "..": never a hit (review G15)
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

// What a lookup found: the entries of each kind asked for, and the notes.
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
    // The walk left dot-names out already, as the filter would.
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

std::expected<SearchResult, std::string> search(const Context& ctx, const SearchRequest& req,
                                                const proc::RunOptions& run,
                                                const CommandHook& on_command) {
    const Mode kind = req.mode;
    auto found = lookup_via_grab(ctx, req, std::span(&kind, 1), search_roots(ctx), req.depth.value_or(ctx.settings.max_depth),
                                 run.stop, on_command);
    if (!found) return util::fail(found.error());
    auto result = make_result(ctx, req, std::move(found->kinds.front()));
    result.notes = std::move(found->notes);
    return result;
}

std::expected<std::pair<SearchResult, SearchResult>, std::string>
search_both(const Context& ctx, const SearchRequest& req, const proc::RunOptions& run, const CommandHook& on_command) {
    auto folders = req;
    folders.mode = Mode::folder;
    auto files = req;
    files.mode = Mode::file;
    constexpr std::array kinds{Mode::folder, Mode::file};
    auto found = lookup_via_grab(ctx, req, kinds, search_roots(ctx), req.depth.value_or(ctx.settings.max_depth), run.stop,
                                 on_command);
    if (!found) return util::fail(found.error());
    auto f = make_result(ctx, folders, std::move(found->kinds[0]));
    auto g = make_result(ctx, files, std::move(found->kinds[1]));
    f.notes = found->notes; // one lookup's, the same for both kinds
    g.notes = std::move(found->notes);
    return std::pair{std::move(f), std::move(g)};
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

} // namespace grab::engine
