#include "engine.hpp"

#include "conn_budget.hpp"
#include "folder.hpp"
#include "gui_state.hpp"
#include "json.hpp"
#include "listing.hpp"
#include "match.hpp"
#include "passwords.hpp"
#include "rclone.hpp"
#include "rclone_encoding.hpp"
#include "rclone_flags.hpp"
#include "rclone_import.hpp"
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
#include <cctype>
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

namespace {

// Why grab's store can't be used for `server`, logging in with `auth`, with what to do about it:
// a change through Edit sets the unreadable file aside (passwords::load_for_change) and starts a
// new one.
std::string store_unreadable(std::string_view why, std::string_view server, server_conf::Auth auth) {
    const std::string edit = std::format("with Edit (Settings → Edit in grab's window, or `grab server edit {}`)", server);
    switch (auth) {
    case server_conf::Auth::password:
        return std::format("{}. Enter the password of {} again {}, which sets the unreadable file aside", why, server,
                           edit);
    case server_conf::Auth::stored_key:
        return std::format("{}. {} logs in with a key kept there: choose a key file or ssh-agent for it {}, which sets "
                           "the unreadable file aside",
                           why, server, edit);
    default: break;
    }
    // ssh-agent or a key file: the store may still keep the server's rclone settings (a proxy, an
    // ssh command), so rclone runs stop as grab's own client does (guard::add_unreadable_store).
    return std::format("{}; it may keep rclone settings of {} (a proxy, an ssh command), so grab doesn't start rclone "
                       "for it. Saving it unchanged {} sets the unreadable file aside",
                       why, server, edit);
}

} // namespace

Context make_context(GrabConfig config, RemoteSettings settings, server_conf::Connection connection,
                     const std::filesystem::path& grab_conf) {
    Context ctx;
    ctx.remote = server_conf::to_remote(settings.rclone_remote, connection, grab_known_hosts_path());
    ctx.method = resolve_find_method(settings, ctx.remote);
    ctx.grab_method = resolve_grab_method(settings, ctx.remote);
    ctx.engine = effective_engine(settings);
    ctx.config = std::move(config);
    ctx.settings = std::move(settings);
    ctx.connection = std::move(connection);
    ctx.passwords = passwords::path_in(state_dir(grab_conf));

    // The guard (phase 8 step 4): the store's rclone extras for this very login, as rclone_env
    // gives them to rclone. A store that exists but can't be read may hold a proxy or an ssh
    // command: the guard then counts the server as routed rather than connect past one.
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
    // ssh.exe find would connect past a route rclone takes.
    if (ctx.engine == Engine::rclone && guard::routed(guard::reasons(ctx.guard))) ctx.method = FindMethod::rclone;
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

    const auto exe_dir = util::self_exe_path().parent_path();
    cfg->rclone = resolve_rclone_exe(*cfg, exe_dir);
    // Asked only where rclone runs: an engine = grab server loads with a missing or too old one.
    if (effective_engine(settings) == Engine::rclone) {
        if (auto usable = check_rclone(cfg->rclone, exe_dir, cfg->rclone != "rclone"); !usable) {
            return util::fail(usable.error());
        }
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
// error for a server that can't log in without it, and `for_rclone` for any: its rclone extras
// are unknown then (rclone_env).
std::expected<passwords::Store, std::string> server_secrets(const Context& ctx, bool for_rclone) {
    if (ctx.passwords.empty()) return passwords::Store{};
    auto store = passwords::load(ctx.passwords);
    if (store) return store;
    const auto auth = ctx.connection.auth;
    if (for_rclone || auth == server_conf::Auth::password || auth == server_conf::Auth::stored_key) {
        return util::fail(store_unreadable(store.error(), ctx.settings.name, auth));
    }
    return passwords::Store{};
}

// Why a --bind can't be used, nullopt when it can: an address goes to rclone and ssh.exe as it
// is, so one that isn't one is refused first.
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
    // A blank --bind ("--bind=") is no bind, as rclone reads it.
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
    const bool on_grab = ctx.engine == Engine::grab;
    if (const auto bad = rclone_flags::first_refused(extra)) {
        // A certificate or an ssh command isn't in grab's own client's settings at all.
        const bool unusable = bad->in_grab == "rclone.pubkey_file" || bad->in_grab == "ssh";
        return util::failf("{} after -- would replace the server's {}, which {}; {}", bad->flag, bad->what,
                           !on_grab   ? "grab gives rclone itself"
                           : unusable ? "grab's own client can't use"
                                      : "grab's own client takes from its settings",
                           rclone_flags::change_instead(*bad, name));
    }
    if (!on_grab) {
        // Anything else would be ignored, or worse ("-- --dry-run" still downloaded the big files, G186).
        if (const auto bad = rclone_flags::first_unusable(extra)) {
            return util::failf("{} after -- isn't used on [{}] (engine = rclone), whose rclone runs take only connection "
                               "flags (--sftp-*, --bind, --timeout...){}",
                               bad->flag, name, bad->hint.empty() ? std::string() : ": " + bad->hint);
        }
    } else if (auto unused = rclone_flags::first_unused_by_grab(extra, name)) {
        // The 2026-10-21 rule: refused, never ignored.
        return util::fail(*unused);
    }
    // The values of the connection flags, on either engine, before anything connects or DEST is
    // made (ssh.exe's searches read them too, through connect_settings).
    const auto given = ssh::read_flags(extra);
    if (!given) return util::failf("the flags after --: {}", given.error());
    if (given->bind && !given->bind->empty()) {
        if (auto bad = bind_problem(*given->bind)) return util::failf("the flags after --: --bind {}: {}", *given->bind, *bad);
    }
    // No limit would leave a stalled READ waiting for ever (ssh_target); rclone takes "off".
    if (on_grab && given->timeout && (*given->timeout < min_io_timeout || *given->timeout > max_io_timeout)) {
        return util::failf("the flags after --: --timeout {}: grab's own client takes a time from 1s to 24h there (with "
                           "none, a stalled download couldn't be cancelled)",
                           given->timeout_text);
    }
    return {};
}

namespace {

// A time as rclone's flags take it: "30s", "1500ms", "0s" for off.
std::string rclone_time(std::chrono::milliseconds t) {
    return t.count() % 1000 == 0 ? std::format("{}s", t.count() / 1000) : std::format("{}ms", t.count());
}

} // namespace

std::vector<std::string> key_flags(const RemoteSettings& settings) {
    std::vector<std::string> out;
    if (settings.bind) out.insert(out.end(), {"--bind", *settings.bind});
    if (settings.connect_timeout) out.insert(out.end(), {"--contimeout", rclone_time(*settings.connect_timeout)});
    if (settings.io_timeout) out.insert(out.end(), {"--timeout", rclone_time(*settings.io_timeout)});
    return out;
}

std::vector<std::string> ssh_connect_options(const ConnectSettings& reach) {
    std::vector<std::string> out;
    if (reach.bind) out.insert(out.end(), {"-o", "BindAddress=" + *reach.bind});
    if (reach.connect_timeout && reach.connect_timeout->count() > 0) {
        const auto seconds = (reach.connect_timeout->count() + 999) / 1000;
        out.insert(out.end(), {"-o", std::format("ConnectTimeout={}", seconds)});
    }
    return out;
}

std::expected<ssh::Target, std::string> ssh_target(const Context& ctx, std::span<const std::string> extra) {
    if (auto pinned = check_pinned(ctx); !pinned) return util::fail(pinned.error());
    // A route, another server, a certificate...: grab's client would connect anyway, past them.
    if (const auto blocked = grab_blockers(ctx, extra); !blocked.empty()) {
        return util::fail(guard::text(ctx.settings.name, blocked, ctx.engine));
    }
    auto store = server_secrets(ctx, false);
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

// A key rclone can read: its options are named in lower case with '_'. A key it would skip in the
// file ("Host", "key-file") must not reach it as RCLONE_CONFIG_<NAME>_HOST or _KEY_FILE either.
bool option_name(std::string_view key) {
    return !key.empty() &&
           std::ranges::all_of(key, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

// The settings whose value rclone expands again (env.ShellExpand: a leading ~, then
// os.ExpandEnv), and the variable a path with a '$' goes in for it.
constexpr auto expanded_paths = std::to_array<std::string_view>({"key_file", "pubkey_file", "known_hosts_file"});
std::string path_variable(std::string_view key) {
    std::string name = "GRAB_RCLONE_";
    for (const char c : key) name += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return name;
}

} // namespace

std::expected<proc::Environment, std::string> rclone_env(const Context& ctx) {
    if (const auto bad = rclone_flags::first_refused(ctx.settings.common_flags)) {
        return util::failf("[{}] common_flags: {} would replace the server's {}, which grab gives rclone itself; "
                           "take it out ({})",
                           ctx.settings.name, bad->flag, bad->what, rclone_flags::change_instead(*bad, ctx.settings.name));
    }
    if (auto pinned = check_pinned(ctx); !pinned) return util::fail(pinned.error());
    // common_flags' --bind reaches rclone as it is: checked as the bind key is.
    if (auto reach = connect_settings(ctx.settings); !reach) return util::fail(reach.error());
    const auto& c = ctx.connection;
    const std::string& name = ctx.settings.name;
    // A store that can't be read stops every login method here: it may keep a proxy or an ssh
    // command rclone would otherwise go without (fail closed, as the guard does for grab's client).
    auto store = server_secrets(ctx, true);
    if (!store) return util::fail(store.error());
    const passwords::Binding login{.user = c.user, .host = c.host, .port = c.port};
    const auto saved = [&](std::string_view key) { return passwords::value_for(*store, name, key, login); };
    // rclone takes a password and a passphrase only obscured.
    const auto obscured = [&](std::string_view plain) -> std::expected<std::string, std::string> {
        auto text = secrets::obscure(plain);
        if (!text) return util::failf("{}: its password couldn't be prepared for rclone (no random numbers)", name);
        return std::move(*text);
    };

    std::map<std::string, std::string> values; // rclone's key -> value
    // The store's rclone extras and the section's rclone.<key> lines first: a key grab sets below
    // wins over them.
    if (const auto it = store->find(name); it != store->end()) {
        for (const auto& [key, secret] : it->second) {
            if (key.starts_with(passwords::rclone_prefix) && secret.binding == login) {
                values[key.substr(passwords::rclone_prefix.size())] = secret.value;
            }
        }
    }
    for (const auto& [key, value] : c.rclone) values[key] = value;
    values["type"] = "sftp";
    values["host"] = c.host;
    values["port"] = std::to_string(c.port);
    values["user"] = c.user;
    switch (c.auth) {
    case server_conf::Auth::agent:
        values["key_use_agent"] = "true";
        if (c.key_file) values["key_file"] = *c.key_file; // rclone then asks the agent for that key alone
        break;
    case server_conf::Auth::key:
        if (c.key_file) values["key_file"] = *c.key_file;
        if (const auto pass = saved(passwords::passphrase)) {
            auto text = obscured(*pass);
            if (!text) return util::fail(text.error());
            values["key_file_pass"] = std::move(*text);
        }
        break;
    case server_conf::Auth::password:
        if (!saved(passwords::password)) {
            return util::failf("{}: {}", name, passwords::why_none(*store, name, passwords::password, login));
        }
        break;
    case server_conf::Auth::stored_key: {
        const auto pem = saved(passwords::key_pem);
        if (!pem) return util::failf("{}: {}", name, passwords::why_none(*store, name, passwords::key_pem, login));
        values["key_pem"] = *pem;
        // rclone takes key_file_pass for key_pem too (backend/sftp), as the import saved it.
        if (const auto pass = saved(passwords::passphrase)) {
            auto text = obscured(*pass);
            if (!text) return util::fail(text.error());
            values["key_file_pass"] = std::move(*text);
        }
        break;
    }
    }
    // A password, also one saved beside a key, which rclone tries after it.
    if (const auto password = saved(passwords::password)) {
        auto text = obscured(*password);
        if (!text) return util::fail(text.error());
        values["pass"] = std::move(*text);
    }
    values["known_hosts_file"] = ctx.remote.known_hosts_file.value_or(util::path_to_utf8(grab_known_hosts_path()));
    // What rclone would otherwise ask the server on every run (RCLONE-REPLACEMENT.md, phase 7
    // chunk A: 0.1 s for the shell, 0.6 s for the sum commands on Main).
    for (const auto& [key, known] : {std::pair{"shell_type", &c.shell_type}, std::pair{"md5sum_command", &c.md5sum_command},
                                     std::pair{"sha1sum_command", &c.sha1sum_command}}) {
        values[key] = known->value_or("none");
    }
    if (c.path_override) values["path_override"] = *c.path_override;

    proc::Environment env;
    env.without_prefix.push_back(secrets::env_prefix(ctx.settings.rclone_remote));
    for (auto& [key, value] : values) {
        if (!option_name(key)) continue;
        if (std::ranges::find(expanded_paths, key) != expanded_paths.end() && value.contains('$')) {
            const auto variable = path_variable(key);
            env.set.emplace_back(variable, std::move(value));
            value = "${" + variable + "}";
        }
        env.set.emplace_back(secrets::env_name(ctx.settings.rclone_remote, key), std::move(value));
    }
    return env;
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
    auto out = run_over_ssh(ctx, build_find_command(find), req.batch_ssh, run, on_command, req.run_flags);
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
    auto with_server = run;
    if (auto env = rclone_env(ctx); env) with_server.environment = std::move(*env);
    else return util::fail(env.error());

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
        list.rclone_remote = ctx.settings.rclone_remote;
        // common_flags', grab's own, the server's keys, then the command's (a --bind reaches searches
        // too, G189).
        list.flags = rclone_flags::connection(ctx.settings.common_flags);
        list.flags.insert(list.flags.end(), req.walk_flags.begin(), req.walk_flags.end());
        const auto keys = key_flags(ctx.settings);
        list.flags.insert(list.flags.end(), keys.begin(), keys.end());
        list.flags.insert(list.flags.end(), req.run_flags.begin(), req.run_flags.end());

        auto out = run_listing(build_lsf_argv(list), with_server, on_command);
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
                                                     const proc::RunOptions& run, const CommandHook& on_command,
                                                     std::span<const std::string> run_flags) {
    if (auto pinned = check_pinned(ctx); !pinned) return util::fail(pinned.error());
    // The server's bind and connect time first: OpenSSH keeps the first value it sees.
    auto reach = connect_settings(ctx.settings, run_flags);
    if (!reach) return util::fail(reach.error());
    std::vector<std::string> ssh_options = ssh_connect_options(*reach);
    ssh_options.insert(ssh_options.end(), ctx.settings.ssh_options.begin(), ctx.settings.ssh_options.end());
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
    auto with_server = run;
    if (auto env = rclone_env(ctx); env) with_server.environment = std::move(*env);
    else return util::fail(env.error());
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
        with_server);
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
