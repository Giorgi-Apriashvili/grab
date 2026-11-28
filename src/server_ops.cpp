#include "server_ops.hpp"

#include "conn_budget.hpp"
#include "engine.hpp"
#include "file_list.hpp"
#include "gui_state.hpp"
#include "ini.hpp"
#include "passwords.hpp"
#include "proxy.hpp"
#include "rclone_import.hpp"
#include "server_conf.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_find.hpp"
#include "ssh_hash.hpp"
#include "ssh_pool.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <system_error>
#include <tuple>
#include <utility>

namespace grab::server_ops {

namespace {

std::expected<void, std::string> write_grab_conf(const Env& env, const std::string& text) {
    if (text == env.grab_text) return {};
    return util::write_file_atomic(env.grab_conf, text);
}

// grab.conf into `env`, its text and settings; a missing file is an empty config. True when the
// file exists.
std::expected<bool, std::string> read_grab_conf(Env& env) {
    auto text = util::read_file_if_exists(env.grab_conf);
    if (!text) return util::fail(text.error());
    if (!*text) {
        env.grab_text = generate_grab_config(nullptr, "");
        env.cfg = GrabConfig{};
        return false;
    }
    env.grab_text = std::move(**text);
    auto doc = ini::parse(env.grab_text);
    if (!doc) return util::failf("{}: {}", util::path_to_utf8(env.grab_conf), doc.error());
    auto cfg = parse_grab_config(*doc);
    if (!cfg) return util::failf("{}: {}", util::path_to_utf8(env.grab_conf), cfg.error());
    env.cfg = std::move(*cfg);
    return true;
}

// For a change: the settings lock, and grab.conf read again under it, so the change starts from
// the file as it is rather than as it was when `loaded` was read; another grab may have changed
// it meanwhile (review G175).
struct Locked {
    SettingsLock lock;
    Env env;
};

std::expected<Locked, std::string> lock_current(const Env& loaded) {
    auto lock = lock_settings();
    if (!lock) return util::fail(lock.error());
    Env env = loaded;
    if (auto read = read_grab_conf(env); !read) return util::fail(read.error());
    return Locked{.lock = std::move(*lock), .env = std::move(env)};
}

// The connection details of server `name` in `text` (grab.conf): nullopt when its section has
// none (not imported yet, or written by hand without them), the error when they can't be read.
std::expected<std::optional<server_conf::Connection>, std::string> connection_in(std::string_view text,
                                                                                 std::string_view name) {
    auto doc = ini::parse(text);
    if (!doc) return util::fail(doc.error());
    const auto* section = doc->find(name);
    if (section == nullptr) return std::nullopt;
    return server_conf::parse(*section);
}

std::vector<std::string> section_names(const Env& env) {
    std::vector<std::string> names;
    if (auto doc = ini::parse(env.grab_text)) {
        for (const auto& s : doc->sections) {
            if (!s.name.empty()) names.push_back(s.name);
        }
    }
    return names;
}

// Whether something but the grab server `except` still needs host:port's keys in grab's
// known_hosts: another grab.conf section at that address (two logins on one machine share its
// host keys), or a remote of an explicit rclone_config whose known_hosts_file is grab's (one
// rclone, run on its own, still checks with them). Host names as written, as known_hosts readers
// compare them. grab's old rclone.conf doesn't count: it still lists every imported server. An
// explicit rclone.conf that can't be read counts as needing them.
bool used_by_another(const Env& env, std::string_view host, int port, std::string_view except) {
    if (auto doc = ini::parse(env.grab_text)) {
        for (const auto& s : doc->sections) {
            if (s.name.empty() || s.name == "grab" || s.name == except) continue;
            const auto c = server_conf::parse(s);
            if (c && *c && (*c)->host == host && (*c)->port == port) return true;
        }
    }
    if (!env.cfg.rclone_config) return false;
    auto text = util::read_file_if_exists(*env.cfg.rclone_config);
    if (!text) return true;
    if (!*text) return false;
    if (is_encrypted_rclone_config(**text)) return true;
    auto doc = ini::parse(**text);
    if (!doc) return true;
    const std::string grab_known = util::to_lower(util::path_to_utf8(env.known_hosts.lexically_normal()));
    for (const auto& section : doc->sections) {
        if (section.name.empty()) continue;
        const auto at = section.get("host");
        if (!at || util::trim(*at) != host) continue;
        int other_port = 22;
        if (const auto p = section.get("port"); p && !util::trim(*p).empty()) {
            const auto v = util::trim(*p);
            if (std::from_chars(v.data(), v.data() + v.size(), other_port).ec != std::errc{}) continue;
        }
        if (other_port != port) continue;
        const auto file = section.get("known_hosts_file");
        if (file && !util::trim(*file).empty() &&
            util::to_lower(util::path_to_utf8(util::path_from_utf8(util::trim(*file)).lexically_normal())) == grab_known) {
            return true;
        }
    }
    return false;
}

// host:port's keys out of grab's known_hosts once nothing needs them but the grab server
// `except`'s (review G87): why not, else "".
std::string forget_unused_keys(const Env& env, std::string_view host, int port, std::string_view except) {
    if (host.empty() || used_by_another(env, host, port, except)) return {};
    auto forgot = forget_host_keys(env.known_hosts, host, port);
    return forgot ? std::string{} : std::format("its host keys could not be removed from {}: {}",
                                                util::path_to_utf8(env.known_hosts), forgot.error());
}

// What grab learned about a server and keeps per name beside its configuration, dropped when the
// server goes or moves to another login (G122): its learned connection limit and the window's
// timings ("<step>:<server>" in gui.json). Why not, else "".
std::string forget_learned(const Env& env, const std::string& name) {
    std::vector<std::string> failed;
    if (auto dropped = save_learned_limit(learned_limits_path(env.grab_conf), name, std::nullopt); !dropped) {
        failed.push_back(std::format("its learned connection limit ({})", dropped.error()));
    }
    const auto gui = state_dir(env.grab_conf) / "gui.json";
    auto state = read_gui_state(gui); // one that can't be read is left alone
    if (state && *state) {
        const auto erased = std::erase_if((*state)->timings, [&](const auto& kv) {
            const auto colon = kv.first.find(':');
            return colon != std::string::npos && std::string_view(kv.first).substr(colon + 1) == name;
        });
        if (erased > 0) {
            if (auto saved = save_gui_state(gui, **state); !saved) {
                failed.push_back(std::format("its timings ({})", saved.error()));
            }
        }
    }
    return failed.empty() ? std::string{} : "grab couldn't forget " + util::join(failed, " and ");
}

// grab's store, for a change, and what the change has to tell. One that can't be read at all is
// an error; one that can't be decrypted or parsed is moved aside (passwords::load_for_change, as
// G211 did for the old store) and the change starts from an empty one, so that a reset Windows
// password never leaves grab without a way out. `note` then names the file it went to and the
// servers but `except` that need their secret again: a password is typed again with Edit, a key
// kept in the store can't be (the import never redoes a server), so that one switches to a key
// file or ssh-agent.
struct StoreForChange {
    passwords::Store store;
    std::string note;
};

std::expected<StoreForChange, std::string> load_store(const Env& env, std::string_view except) {
    auto loaded = passwords::load_for_change(env.passwords);
    if (!loaded) {
        return util::failf("grab's saved passwords ({}) can't be read: {}", util::path_to_utf8(env.passwords),
                           loaded.error());
    }
    StoreForChange out{.store = std::move(loaded->store), .note = {}};
    if (!loaded->set_aside) return out;
    std::vector<std::string> typed;
    std::vector<std::string> stored_key;
    for (const auto& name : section_names(env)) {
        if (name == "grab" || name == except) continue;
        const auto c = connection_in(env.grab_text, name);
        if (!c || !*c) continue;
        if ((*c)->auth == servers::Auth::password) typed.push_back(name);
        if ((*c)->auth == servers::Auth::stored_key) stored_key.push_back(name);
    }
    out.note = std::format("grab's saved passwords couldn't be read (another Windows account's, or damaged) and were "
                           "moved aside to {}, with any proxy or ssh command a server kept there (such a server now "
                           "connects directly, or through the proxy its grab.conf section gives it, whose password "
                           "is typed again with Edit)",
                           util::path_to_utf8(*loaded->set_aside));
    if (!typed.empty()) out.note += std::format("; enter the password of {} again with Edit", util::join(typed, ", "));
    if (!stored_key.empty()) {
        out.note += std::format("; {} logged in with a key kept in grab's store: choose a key file or ssh-agent for {} "
                                "with Edit",
                                util::join(stored_key, ", "), stored_key.size() == 1 ? "it" : "them");
    }
    return out;
}

// The grab.conf keys of `c` set in [name], each only when it differs; a key `c` leaves out is
// blanked where the section has it. A key the section lacked goes after the one before it in
// server_conf::keys' order (after the header when it has none), so they stay in that order.
std::expected<std::string, std::string> set_connection(std::string text, std::string_view name,
                                                       const server_conf::Connection& c) {
    auto doc = ini::parse(text);
    if (!doc) return util::fail(doc.error());
    const auto* section = doc->find(name);
    const auto wanted = server_conf::lines(c);
    std::map<std::string, std::string, std::less<>> values(wanted.begin(), wanted.end());
    const std::span<const std::string_view> keys = server_conf::keys;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const std::string_view key = keys[i];
        const auto found = values.find(key);
        const std::string value = found == values.end() ? std::string() : found->second;
        const auto now = section != nullptr ? section->get(key) : std::nullopt;
        if (util::trim(now.value_or("")) == value || (value.empty() && !now)) continue;
        auto set = servers::set_value(text, name, key, value, keys.first(i));
        if (!set) return util::fail(set.error());
        text = std::move(*set);
    }
    return text;
}

server_conf::Connection connection_of(const servers::NewServer& s) {
    server_conf::Connection c;
    c.host = s.host;
    c.port = s.port;
    c.user = s.user;
    c.auth = s.auth;
    if ((s.auth == servers::Auth::key || s.auth == servers::Auth::agent) && !s.key_file.empty()) c.key_file = s.key_file;
    return c;
}

// ---- proxies (PROXIES.md 3.3) -----------------------------------------------------------------

// The proxy of server `name`'s `proxy` line in `env`'s grab.conf, read whether or not the rest of
// its section can be: kind none when it has none, the error ("[Box] proxy: ...") when grab can't
// read it.
std::expected<proxy::Proxy, std::string> line_proxy(const Env& env, std::string_view name) {
    auto doc = ini::parse(env.grab_text);
    if (!doc) return util::fail(doc.error());
    const auto* section = doc->find(name);
    if (section == nullptr) return proxy::Proxy{};
    const auto line = section->get("proxy");
    if (!line || util::trim(*line).empty()) return proxy::Proxy{};
    auto p = proxy::parse(util::trim(*line), name);
    if (!p) return util::failf("[{}] proxy: {}", name, p.error());
    return p;
}

constexpr std::string_view socks_copy = "rclone.socks_proxy";
constexpr std::string_view http_copy = "rclone.http_proxy";
constexpr std::array<std::string_view, 3> proxy_keys{passwords::proxy_password, socks_copy, http_copy};

using Values = std::map<std::string, passwords::Secret, std::less<>>;

// What the store keeps of server `name`'s proxy `p` (with its password; none: directly), bound to
// `login`, the server's: the proxy password, only with a user, bound to the proxy's own
// user@host:port; the copy for older grabs in rclone's form (decision 6), bound to the login, as
// the import kept one, so that 0.21-0.25 refuse the server and 0.20.1 goes through it.
Values proxy_entries(const proxy::Proxy& p, const passwords::Binding& login) {
    Values out;
    if (p.kind == proxy::Kind::none) return out;
    if (!p.user.empty() && !p.password.empty()) {
        out.emplace(passwords::proxy_password, passwords::Secret{.binding = guard::proxy_login(p), .value = p.password});
    }
    out.emplace(p.kind == proxy::Kind::socks5 ? socks_copy : http_copy,
                passwords::Secret{.binding = login, .value = proxy::rclone_text(p)});
    return out;
}

// Whether `store` holds exactly `entries` of server `name`'s proxy keys.
bool holds_entries(const passwords::Store& store, std::string_view name, const Values& entries) {
    return std::ranges::all_of(proxy_keys, [&](std::string_view key) {
        const auto* have = passwords::find(store, name, key);
        const auto want = entries.find(key);
        return want == entries.end() ? have == nullptr : have != nullptr && *have == want->second;
    });
}

// Server `name`'s proxy keys in `store` replaced by `entries`.
void set_entries(passwords::Store& store, const std::string& name, const Values& entries) {
    auto& values = store[name];
    for (const std::string_view key : proxy_keys) {
        if (const auto it = values.find(key); it != values.end()) values.erase(it);
    }
    for (const auto& [key, secret] : entries) values.insert_or_assign(key, secret);
    if (values.empty()) store.erase(name);
}

bool same_proxy(const proxy::Proxy& a, const proxy::Proxy& b) {
    return a.kind == b.kind && a.host == b.host && a.port == b.port && a.user == b.user;
}

// The proxy server `wanted.name` has after an edit to `wanted`, with the password its store entries
// then hold: the wanted one (none: directly), its password typed, else the one saved for that
// very proxy and user; unset, its `proxy` line (`line`), else the one proxy the store keeps for it
// in rclone's form, of any login (the edit binds it to the new one), which moves into the line
// (3.7). nullopt: the store's proxies stay as they are (it keeps none, or ones grab won't use,
// which go on refusing the server). A line that can't be read offers no saved password (Edit's
// prompt then says Enter means none), so none is taken from the store's copy either.
std::optional<proxy::Proxy> proxy_after(const passwords::Store& store, const servers::NewServer& wanted,
                                        const std::expected<proxy::Proxy, std::string>& line) {
    if (!line) {
        if (!wanted.proxy) return std::nullopt; // the save refuses this (update_server)
        if (wanted.proxy->kind == proxy::Kind::none) return proxy::Proxy{};
        proxy::Proxy p = *wanted.proxy;
        if (p.user.empty()) p.password.clear(); // sent only with a user
        return p;
    }
    server_conf::Connection now;
    now.proxy = *line;
    auto current = guard::route(store, wanted.name, now, true);
    if (wanted.proxy) {
        proxy::Proxy p = *wanted.proxy;
        if (p.kind == proxy::Kind::none) return proxy::Proxy{};
        if (p.password.empty() && current && same_proxy(current->proxy, p)) p.password = current->proxy.password;
        if (p.user.empty()) p.password.clear(); // sent only with a user
        return p;
    }
    if (!current || current->from == guard::ProxyFrom::none) return std::nullopt;
    return std::move(current->proxy);
}

} // namespace

std::expected<Env, std::string> load_env(const std::filesystem::path& grab_conf) {
    Env env;
    env.grab_conf = grab_conf;
    if (auto applied = rclone_import::apply(rclone_import::files_for(grab_conf)); !applied) {
        env.import_notes.push_back(applied.error());
    } else {
        for (const auto& server : applied->imported) env.imported.push_back(server.name);
        for (const auto& note : applied->notes) env.import_notes.push_back(std::format("[{}] {}", note.server, note.text));
        env.old_files = std::move(applied->old_files);
        env.old_files_recorded = applied->old_files_recorded;
    }
    if (auto exists = read_grab_conf(env); !exists) return util::fail(exists.error());
    env.known_hosts = grab_known_hosts_path();
    env.passwords = passwords::path_in(state_dir(grab_conf));
    return env;
}

// ---- lists ---------------------------------------------------------------------------------

bool is_default_server(const GrabConfig& cfg, std::string_view name) {
    if (cfg.default_remote) return *cfg.default_remote == name;
    return cfg.remotes.size() == 1 && cfg.remotes.front().name == name;
}

const RemoteSettings* find_server(const GrabConfig& cfg, std::string_view name) {
    for (const auto& r : cfg.remotes) {
        if (r.name == name) return &r;
    }
    return nullptr;
}

namespace {

// A time as grab.conf takes it: "30s", "1500ms", "off".
std::string time_text(std::chrono::milliseconds t) {
    if (t.count() == 0) return "off";
    return t.count() % 1000 == 0 ? std::format("{}s", t.count() / 1000) : std::format("{}ms", t.count());
}

// The guard's reasons, the proxy (without its password) and the search method of server `s` as
// make_context and ssh_target resolve them (a `store` that can't be read may hold a route; its
// proxy is then the line's).
struct Resolved {
    std::vector<guard::Reason> guard;
    std::string search;
    proxy::Proxy proxy;
    guard::ProxyFrom proxy_from = guard::ProxyFrom::none;
};
Resolved resolve(const RemoteSettings& s, const server_conf::Connection& conn, const RcloneRemote& remote,
                 const std::expected<passwords::Store, std::string>& store) {
    const passwords::Binding login{.user = conn.user, .host = conn.host, .port = conn.port};
    auto sources = guard::sources(s, conn, store ? guard::store_extras(*store, s.name, login)
                                                 : std::vector<std::pair<std::string, std::string>>{});
    auto found = guard::evaluate(sources);
    // Its copy of the extras (a proxy can hold a password), wiped as make_context does.
    for (auto& [key, value] : sources.extras) std::ranges::fill(value, '\0');
    Resolved out;
    out.proxy = conn.proxy;
    out.proxy_from = conn.proxy.kind == proxy::Kind::none ? guard::ProxyFrom::none : guard::ProxyFrom::line;
    if (!store) {
        guard::add_unreadable_store(found);
    } else if (auto route = guard::route(*store, s.name, conn); !route) {
        guard::add_route_problem(found, route.error());
    } else {
        out.proxy = std::move(route->proxy);
        out.proxy_from = route->from;
    }
    std::ranges::fill(out.proxy.password, '\0');
    out.proxy.password.clear();
    out.guard = guard::reasons(found);
    out.search = std::string(search_method(s, remote));
    return out;
}

// What grab's comment above a section says how it logs in, when the remote can't say it.
std::string_view login_said(const server_conf::Connection& conn) {
    return conn.auth == servers::Auth::stored_key ? "key in grab's store" : "";
}

} // namespace

std::vector<ServerInfo> list_servers(const Env& env) {
    std::vector<ServerInfo> out;
    // Read when asked (G209): pinned is what the file holds now.
    const std::string known = util::read_file_if_exists(env.known_hosts).value_or(std::nullopt).value_or("");
    const auto store = passwords::load(env.passwords); // only what is saved is said, never a value
    for (const auto& s : env.cfg.remotes) {
        ServerInfo info;
        info.name = s.name;
        info.rclone_remote = s.rclone_remote;
        info.is_default = is_default_server(env.cfg, s.name);
        for (const auto& r : s.search_roots) info.search_roots.push_back(r.empty() ? "~" : r);
        info.max_depth = s.max_depth;
        info.max_connections = s.max_connections;
        std::vector<std::string> keys;
        if (s.bind) keys.push_back("bind " + *s.bind);
        if (s.connect_timeout) keys.push_back("connect_timeout " + time_text(*s.connect_timeout));
        if (s.io_timeout) keys.push_back("io_timeout " + time_text(*s.io_timeout));
        info.connection_keys = util::join(keys, ", ");
        auto c = connection_in(env.grab_text, s.name);
        if (!c) {
            info.error = c.error();
        } else if (!*c) {
            info.error = std::format("[{}] has no connection details (host, port, user, auth) in grab.conf: fill them in "
                                     "with Edit (grab server edit {})",
                                     s.name, s.name);
            // Why the import couldn't fill them in, when this load tried.
            const std::string tag = std::format("[{}] ", s.name);
            for (const auto& note : env.import_notes) {
                if (note.starts_with(tag + "not imported")) info.error += " (" + note.substr(tag.size()) + ")";
            }
        } else {
            const auto& conn = **c;
            const RcloneRemote remote = server_conf::to_remote(s.rclone_remote, conn, env.known_hosts);
            info.host = conn.host;
            info.user = conn.user;
            info.port = conn.port;
            info.auth = conn.auth;
            info.key_file = conn.key_file.value_or("");
            info.host_key_pinned = server_conf::pinned(known, conn.host, conn.port);
            info.default_connections = budget::default_budget(conn.host);
            const std::string_view key = conn.auth == servers::Auth::password ? passwords::password
                                         : conn.auth == servers::Auth::key    ? passwords::passphrase
                                         : conn.auth == servers::Auth::stored_key ? passwords::key_pem
                                                                                  : std::string_view{};
            const passwords::Binding login{.user = conn.user, .host = conn.host, .port = conn.port};
            if (!key.empty() && store) info.secret_saved = passwords::value_for(*store, s.name, key, login).has_value();
            if (!store) info.store_error = store.error();
            auto resolved = resolve(s, conn, remote, store);
            info.guard = std::move(resolved.guard);
            info.search = std::move(resolved.search);
            info.proxy = std::move(resolved.proxy);
            info.proxy_from = resolved.proxy_from;
            if (!info.guard.empty()) {
                info.unusable = guard::text(s.name, info.guard);
            } else if (auto reach = engine::connect_settings(s); !reach) {
                info.unusable = reach.error();
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

// ---- host keys -----------------------------------------------------------------------------

std::expected<HostKeys, std::string> scan_host_keys(const std::string& host, int port, std::stop_token stop,
                                                    const ssh::Reach& reach) {
    auto scanned = ssh::scan_host_keys(host, port, std::chrono::seconds(10), std::move(stop), reach);
    if (!scanned) return util::fail(scanned.error());
    HostKeys keys;
    for (const auto& key : *scanned) {
        keys.lines.push_back(ssh::known_host_line(host, port, key));
        keys.fingerprints.push_back(servers::Fingerprint{.hash = key.fingerprint, .type = servers::key_label(key.type)});
    }
    return keys;
}

namespace {

// What keeps server `name` off grab: for one with connection details, what list_servers says
// (resolve); for a section without them, its own settings and the environment; for a server
// grab.conf doesn't have yet, the environment alone (an inherited RCLONE_SFTP_* covers every
// server). A store that can't be read counts only beside details: what it keeps is bound to a login.
// For an Edit's fetch, what the save will leave (reach_for); `proxy_given`: the Edit gives the
// proxy, which replaces any the store keeps (reach_for_save).
std::vector<guard::Reason> guard_of(const Env& env, std::string_view name, Purpose purpose, bool proxy_given = false) {
    const auto* s = find_server(env.cfg, name);
    if (s != nullptr && purpose == Purpose::edit) {
        // The section's rclone.* lines as they are: set_connection keeps them, read or not.
        server_conf::Connection lines;
        if (auto doc = ini::parse(env.grab_text)) {
            if (const auto* section = doc->find(name)) {
                for (const auto& [key, value] : section->values) {
                    const auto v = util::trim(value);
                    if (key.starts_with(server_conf::rclone_prefix) && !v.empty()) {
                        lines.rclone.emplace_back(key.substr(server_conf::rclone_prefix.size()), v);
                    }
                }
            }
        }
        std::vector<std::pair<std::string, std::string>> extras;
        std::optional<guard::RouteProblem> problem;
        if (const auto store = passwords::load(env.passwords)) {
            if (const auto it = store->find(name); it != store->end()) {
                for (const auto& [key, saved] : it->second) {
                    if (key.starts_with(passwords::rclone_prefix)) {
                        extras.emplace_back(key.substr(passwords::rclone_prefix.size()), saved.value);
                    }
                }
            }
            // The kept proxies of any login, unless the `proxy` line wins (one grab can't read
            // is reach_for's to refuse) or the Edit gives its own.
            lines.proxy = line_proxy(env, name).value_or(proxy::Proxy{});
            if (auto route = guard::route(*store, name, lines, true); !route && !proxy_given) {
                problem = std::move(route.error());
            }
        }
        auto sources = guard::sources(*s, lines, std::move(extras));
        auto found = guard::evaluate(sources);
        for (auto& [key, value] : sources.extras) std::ranges::fill(value, '\0');
        if (problem) guard::add_route_problem(found, *problem);
        return guard::reasons(found);
    }
    if (s != nullptr) {
        if (const auto c = connection_in(env.grab_text, name); c && *c) {
            const RcloneRemote remote = server_conf::to_remote(s->rclone_remote, **c, env.known_hosts);
            return resolve(*s, **c, remote, passwords::load(env.passwords)).guard;
        }
    }
    RemoteSettings settings;
    if (s != nullptr) settings = *s;
    return guard::reasons(guard::evaluate(guard::sources(settings, server_conf::Connection{}, {})));
}

} // namespace

std::expected<ssh::Reach, std::string> reach_for(const Env& env, std::string_view name, Purpose purpose) {
    // Not even its host key is fetched (phase 9 step 6).
    if (const auto blocked = guard_of(env, name, purpose); !blocked.empty()) {
        return util::fail(guard::text(name, blocked));
    }
    const auto* s = find_server(env.cfg, name);
    if (s == nullptr) return ssh::Reach{};
    auto reach = engine::connect_settings(*s);
    if (!reach) return util::fail(reach.error());
    ssh::Reach out;
    if (reach->bind) {
        out.bind = *reach->bind;
        out.bind_from = reach->bind_from;
    }
    out.connect_timeout = reach->connect_timeout;

    // Its proxy as its connections take it (guard::route), or as an Edit leaves it; never around
    // a `proxy` line grab can't read.
    const auto line = line_proxy(env, name);
    if (!line) return util::fail(line.error());
    server_conf::Connection c;
    const auto details = connection_in(env.grab_text, name);
    const bool known = details && *details;
    if (known) c = **details;
    c.proxy = *line;
    if (const auto store = passwords::load(env.passwords)) {
        // Without details there is no login to match: a kept proxy of any login goes, rather than
        // none (the guard has refused what Trust won't use before this).
        auto route = guard::route(*store, name, c, purpose == Purpose::edit || !known);
        if (!route) return util::fail(guard::route_text(name, route.error()));
        out.proxy = std::move(route->proxy);
    } else if (purpose == Purpose::edit) {
        // An Edit sets a store that can't be read aside, and what it kept with it.
        out.proxy = *line;
    } else {
        // Trust is refused for a store that can't be read (the guard), also one that failed since
        // the guard read it: it may keep the server's proxy.
        guard::Settings unreadable;
        guard::add_unreadable_store(unreadable);
        return util::fail(guard::text(name, guard::reasons(unreadable)));
    }
    if (!out.connect_timeout && out.proxy.kind != proxy::Kind::none) out.connect_timeout = engine::proxy_connect_timeout;
    return out;
}

std::expected<ssh::Reach, std::string> reach_for_save(const Env& env, const servers::NewServer& wanted) {
    if (!wanted.proxy) return reach_for(env, wanted.name, Purpose::edit);
    if (const auto blocked = guard_of(env, wanted.name, Purpose::edit, true); !blocked.empty()) {
        return util::fail(guard::text(wanted.name, blocked));
    }
    ssh::Reach out;
    out.proxy = *wanted.proxy;
    if (const auto* s = find_server(env.cfg, wanted.name)) {
        auto reach = engine::connect_settings(*s);
        if (!reach) return util::fail(reach.error());
        if (reach->bind) {
            out.bind = *reach->bind;
            out.bind_from = reach->bind_from;
        }
        out.connect_timeout = reach->connect_timeout;
        // The password as the save keeps it (proxy_after): typed, else the one saved for that
        // very proxy and user. A store that can't be read is set aside by the save, with it.
        if (const auto store = passwords::load(env.passwords)) {
            out.proxy = proxy_after(*store, wanted, line_proxy(env, wanted.name)).value_or(proxy::Proxy{});
        }
    }
    // A new server's name may still have what a removed one left in the store: Add never uses it.
    if (out.proxy.user.empty()) out.proxy.password.clear(); // sent only with a user
    if (!out.connect_timeout && out.proxy.kind != proxy::Kind::none) out.connect_timeout = engine::proxy_connect_timeout;
    return out;
}

namespace {

// known_hosts changed by `edit` under the settings lock, written in one step when it changed. One
// that can't be read is never replaced: every other server's pins would go with it (review G89).
std::expected<void, std::string> edit_known_hosts(const std::filesystem::path& known_hosts,
                                                  const std::function<std::string(std::string_view)>& edit) {
    auto lock = lock_settings();
    if (!lock) return util::fail(lock.error());
    auto existing = util::read_file_if_exists(known_hosts);
    if (!existing) return util::fail(existing.error());
    const std::string before = existing->value_or("");
    std::string after = edit(before);
    if (after == before && (existing->has_value() || after.empty())) return {}; // nothing to write
    return util::write_file_atomic(known_hosts, after);
}

} // namespace

std::expected<void, std::string> pin_host_keys(const std::filesystem::path& known_hosts, std::string_view host,
                                               int port, const std::vector<std::string>& lines) {
    if (lines.empty()) return util::failf("no host keys of {}:{} to pin", host, port);
    for (const auto& line : lines) {
        const auto t = util::trim(line);
        const auto names = util::split(t.substr(0, t.find_first_of(" \t")), ',');
        if (std::ranges::none_of(names, [&](const std::string& n) { return servers::names_host(n, host, port); })) {
            return util::failf("the host keys to pin aren't {}:{}'s; read them again", host, port);
        }
    }
    return edit_known_hosts(known_hosts, [&](std::string_view text) {
        return servers::merge_known_hosts(servers::forget_host(text, host, port), lines);
    });
}

std::expected<void, std::string> forget_host_keys(const std::filesystem::path& known_hosts, std::string_view host,
                                                  int port) {
    return edit_known_hosts(known_hosts, [&](std::string_view text) { return servers::forget_host(text, host, port); });
}

std::optional<std::string> name_taken(const Env& env, const std::string& name) {
    return server_conf::check_new_name(name, section_names(env));
}

std::optional<Problem> check_server(const servers::NewServer& s) {
    if (auto why = server_conf::check_host(s.host)) return Problem{.field = "host", .why = std::move(*why)};
    if (auto why = server_conf::check_port(s.port)) return Problem{.field = "port", .why = std::move(*why)};
    if (auto why = server_conf::check_user(s.user)) return Problem{.field = "user", .why = std::move(*why)};
    if (s.auth == servers::Auth::key || (s.auth == servers::Auth::agent && !s.key_file.empty())) {
        std::error_code ec;
        const bool there = std::filesystem::is_regular_file(util::path_from_utf8(s.key_file), ec);
        if (auto why = server_conf::check_key_file(s.key_file, there)) return Problem{.field = "keyFile", .why = std::move(*why)};
    }
    if (s.proxy && s.proxy->kind != proxy::Kind::none) {
        const proxy::Proxy& p = *s.proxy;
        if (auto why = server_conf::check_proxy(p, s.name)) return Problem{.field = "proxy", .why = std::move(*why)};
        if (s.host.contains('%')) {
            return Problem{.field = "host", .why = "an address with a zone can't go through a proxy"};
        }
        if (p.kind == proxy::Kind::socks5 && p.password.size() > 255) {
            return Problem{.field = "proxySecret",
                           .why = "the proxy's password is longer than SOCKS5 allows (255 bytes)"};
        }
    }
    for (const auto& root : s.search_roots) {
        if (auto why = server_conf::check_search_root(root)) return Problem{.field = "roots", .why = std::move(*why)};
    }
    return std::nullopt;
}

// ---- changes -------------------------------------------------------------------------------

std::expected<std::string, std::string> add_server(const Env& loaded, const servers::NewServer& s,
                                                   const std::optional<std::string>& secret, const HostKeys& keys) {
    auto locked = lock_current(loaded);
    if (!locked) return util::fail(locked.error());
    const Env& env = locked->env;
    // The name once more, now that no other change can come in between.
    if (auto why = name_taken(env, s.name)) return util::fail(*why);
    if (auto problem = check_server(s)) return util::failf("{}: {}", problem->field, problem->why);
    if (s.auth == servers::Auth::stored_key) return util::fail("a key kept in grab's store can only be imported");
    if (s.auth == servers::Auth::password && (!secret || secret->empty())) return util::fail("enter the password");
    server_conf::Connection c = connection_of(s);
    if (s.proxy) {
        c.proxy = *s.proxy;
        c.proxy.password.clear(); // the store's, never grab.conf's
    }

    if (auto pinned = pin_host_keys(env.known_hosts, s.host, s.port, keys.lines); !pinned) {
        return util::fail(pinned.error());
    }
    // Its secret, in place of anything a server of that name left in the store.
    auto loaded_store = load_store(env, s.name);
    if (!loaded_store) return util::fail(loaded_store.error());
    const std::string note = std::move(loaded_store->note);
    const passwords::Store before = std::move(loaded_store->store);
    passwords::Store store = before;
    store.erase(s.name);
    const passwords::Binding login{.user = c.user, .host = c.host, .port = c.port};
    if (secret && !secret->empty() && (s.auth == servers::Auth::password || s.auth == servers::Auth::key)) {
        const auto key = s.auth == servers::Auth::password ? passwords::password : passwords::passphrase;
        store[s.name][std::string(key)] = passwords::Secret{.binding = login, .value = *secret};
    }
    // Its proxy's password and the copy for older grabs.
    if (s.proxy) {
        proxy::Proxy p = *s.proxy;
        if (p.user.empty()) p.password.clear(); // sent only with a user
        set_entries(store, s.name, proxy_entries(p, login));
    }
    if (store != before) {
        if (auto saved = passwords::save(env.passwords, store); !saved) return util::fail(saved.error());
    }
    // From here a failure puts the store back, so that the name can be added once more (G218).
    auto undo = [&](std::string why) {
        if (store != before) {
            if (auto back = passwords::save(env.passwords, before); !back) {
                why += std::format("; its saved password could not be removed again: {}", back.error());
            }
        }
        if (!note.empty()) why += "; " + note;
        return util::fail(std::move(why));
    };

    const RcloneRemote remote = server_conf::to_remote(s.name, c, env.known_hosts);
    std::string text = servers::append_section(env.grab_text,
                                               remote_section(remote, s.search_roots, s.max_depth, server_conf::lines(c)));
    std::expected<std::string, std::string> set = text;
    if (s.max_connections) set = servers::set_value(text, s.name, "max_connections", std::to_string(*s.max_connections));
    if (set && (!env.cfg.default_remote || env.cfg.remotes.empty())) {
        set = servers::set_value(*set, "grab", "default_remote", s.name);
    }
    if (!set) return undo(set.error());
    if (auto written = write_grab_conf(env, *set); !written) return undo(written.error());
    return note;
}

servers::NewServer edit_defaults(const ServerInfo& current) {
    servers::NewServer s;
    s.name = current.name;
    if (current.error.empty()) {
        s.host = current.host;
        s.port = current.port;
        s.user = current.user;
        s.auth = current.auth;
        s.key_file = current.key_file;
    }
    for (const auto& root : current.search_roots) s.search_roots.push_back(normalize_root(root));
    s.max_depth = current.max_depth;
    s.max_connections = current.max_connections;
    return s;
}

ProxyOffer edit_proxy(const Env& env, std::string_view name) {
    const auto line = line_proxy(env, name);
    if (!line) {
        return ProxyOffer{.proxy = std::nullopt, .from = guard::ProxyFrom::none, .password_saved = false,
                          .why = line.error()};
    }
    const auto from_line = line->kind == proxy::Kind::none ? guard::ProxyFrom::none : guard::ProxyFrom::line;
    const auto store = passwords::load(env.passwords);
    if (!store) return ProxyOffer{.proxy = *line, .from = from_line, .password_saved = false, .why = {}};
    // As proxy_after finds it for a save that leaves the proxy unset.
    server_conf::Connection now;
    now.proxy = *line;
    auto route = guard::route(*store, name, now, true);
    if (!route) {
        return ProxyOffer{.proxy = std::nullopt, .from = guard::ProxyFrom::none, .password_saved = false,
                          .why = guard::route_text(name, route.error())};
    }
    ProxyOffer out{
        .proxy = route->proxy, .from = route->from, .password_saved = !route->proxy.password.empty(), .why = {}};
    std::ranges::fill(route->proxy.password, '\0');
    std::ranges::fill(out.proxy->password, '\0');
    out.proxy->password.clear();
    return out;
}

bool edit_needs_host_key(const ServerInfo& current, const servers::NewServer& wanted) {
    return !current.error.empty() || !current.host_key_pinned || current.host != wanted.host ||
           current.port != wanted.port;
}

bool edit_needs_password(const ServerInfo& current, const servers::NewServer& wanted) {
    if (wanted.auth != servers::Auth::password) return false;
    // As the store's binding compares them: exactly.
    return !current.error.empty() || current.auth != servers::Auth::password || !current.secret_saved.value_or(false) ||
           current.host != wanted.host || current.port != wanted.port || current.user != wanted.user;
}

std::expected<std::string, std::string> update_server(const Env& loaded, const servers::NewServer& wanted,
                                                      const std::optional<std::string>& secret,
                                                      const std::optional<HostKeys>& keys) {
    auto locked = lock_current(loaded);
    if (!locked) return util::fail(locked.error());
    const Env& env = locked->env;
    const auto* settings = find_server(env.cfg, wanted.name);
    if (settings == nullptr) return util::failf("no server called {} in grab.conf", wanted.name);
    if (auto problem = check_server(wanted)) return util::failf("{}: {}", problem->field, problem->why);
    // Details that can't be read are replaced, as if there were none.
    const auto read = connection_in(env.grab_text, wanted.name);
    const std::optional<server_conf::Connection> current = read ? *read : std::nullopt;
    const bool typed = secret && !secret->empty();
    // The `proxy` line, read whatever else of the section can't be: one grab can't read stops the
    // save unless it sets another, rather than blank it (the server would connect directly).
    const auto line = line_proxy(env, wanted.name);
    if (!line && !wanted.proxy) {
        return util::failf("{}; correct the proxy line in [{}] (grab config) first", line.error(), wanted.name);
    }
    const auto readable = passwords::load(env.passwords);
    // The proxy the save leaves, with the password the store then keeps for it (an unreadable
    // store is set aside below: nothing of it is kept).
    const auto proxy_next = proxy_after(readable ? *readable : passwords::Store{}, wanted, line);

    server_conf::Connection next = current.value_or(server_conf::Connection{});
    next.host = wanted.host;
    next.port = wanted.port;
    next.user = wanted.user;
    next.auth = wanted.auth;
    next.proxy = proxy_next.value_or(proxy::Proxy{});
    next.proxy.password.clear(); // the store's, never grab.conf's
    if (wanted.auth == servers::Auth::stored_key && (!current || current->auth != servers::Auth::stored_key)) {
        return util::fail("a key kept in grab's store can only be imported: choose another way to log in");
    }
    // The key file of `key`; beside `agent` (which agent key) the one given, else the one it had.
    const bool agent_kept = wanted.auth == servers::Auth::agent && wanted.key_file.empty() && current &&
                            current->auth == servers::Auth::agent;
    if (!agent_kept) next.key_file = connection_of(wanted).key_file;
    const bool moved = !current || current->host != next.host || current->port != next.port || current->user != next.user;
    const bool method = !current || current->auth != next.auth;
    // Another key file: the old one's passphrase goes unless a new one is typed (it was the old
    // key's; 0.20's rclone refused an unencrypted key given one).
    const bool key_changed = current && current->auth == servers::Auth::key && next.auth == servers::Auth::key &&
                             current->key_file != next.key_file;
    if (moved) {
        // What the old server answered (ssh::learn_commands): the next Test asks the new one.
        next.shell_type.reset();
        next.md5sum_command.reset();
        next.sha1sum_command.reset();
    }
    if (wanted.auth == servers::Auth::password && !typed && (moved || method)) {
        return util::fail("enter the password again: it is only kept for the same user, host and port");
    }
    if (auto bad = server_conf::check(wanted.name, next); !bad.empty()) return util::fail(bad.front());

    if (keys) {
        if (auto pinned = pin_host_keys(env.known_hosts, wanted.host, wanted.port, keys->lines); !pinned) {
            return util::fail(pinned.error());
        }
    }

    // The store, only when the login method, the secret or the login changes (G85), or when it
    // can't be read: grab then connects to no server, and saving one with Edit, changed or not, is
    // the way out the messages give (load_store sets the file aside). A store that still can't be
    // read stops only an Edit that needs it.
    std::optional<passwords::Store> before;
    passwords::Store store;
    std::string store_note;
    const passwords::Binding login{.user = next.user, .host = next.host, .port = next.port};
    // The proxy's password and the copy for older grabs, kept in step with the line.
    const Values entries = proxy_next ? proxy_entries(*proxy_next, login) : Values{};
    const bool proxy_changes = proxy_next && !(readable && holds_entries(*readable, wanted.name, entries));
    const bool secret_kind = wanted.auth == servers::Auth::password || wanted.auth == servers::Auth::key;
    const bool needs_store = method || moved || key_changed || (typed && secret_kind) || proxy_changes;
    const bool unreadable = !readable.has_value();
    std::optional<StoreForChange> loaded_store;
    if (needs_store || unreadable) {
        auto got = load_store(env, wanted.name);
        if (!got && needs_store) return util::fail(got.error());
        if (got) loaded_store = std::move(*got);
    }
    if (loaded_store) {
        store_note = std::move(loaded_store->note);
        before = loaded_store->store;
        store = std::move(loaded_store->store);
        if (const auto it = store.find(wanted.name); it != store.end()) {
            auto& values = it->second;
            if (method) {
                // The old method's secrets go; rclone extras stay.
                for (const std::string_view key : {passwords::password, passwords::passphrase, passwords::key_pem}) {
                    const bool kept = (key == passwords::passphrase && next.auth == servers::Auth::key) ||
                                      (key == passwords::password && next.auth == servers::Auth::password);
                    if (!kept) values.erase(std::string(key));
                }
            }
            // A password goes only to the login it was typed for (G40); the key's passphrase, an
            // imported key and rclone extras belong to this server wherever it moved. The proxy's
            // password stays bound to its proxy.
            if (moved) values.erase(std::string(passwords::password));
            if (key_changed && !typed) values.erase(std::string(passwords::passphrase));
            for (auto& [key, saved] : values) {
                if (key != passwords::proxy_password) saved.binding = login;
            }
            if (values.empty()) store.erase(it);
        }
        if (typed && secret_kind) {
            const auto key = wanted.auth == servers::Auth::password ? passwords::password : passwords::passphrase;
            store[wanted.name][std::string(key)] = passwords::Secret{.binding = login, .value = *secret};
        }
        if (proxy_next) set_entries(store, wanted.name, entries);
        if (store != *before) {
            if (auto saved = passwords::save(env.passwords, store); !saved) return util::fail(saved.error());
        }
    }
    auto undo = [&](std::string why) {
        if (before && store != *before) {
            if (auto back = passwords::save(env.passwords, *before); !back) {
                why += std::format("; the saved passwords could not be put back: {}", back.error());
            }
        }
        if (!store_note.empty()) why += "; " + store_note;
        return util::fail(std::move(why));
    };

    auto text = set_connection(env.grab_text, wanted.name, next);
    if (text) text = servers::set_value(*text, wanted.name, "search_roots", search_roots_value(wanted.search_roots));
    if (text) text = servers::set_value(*text, wanted.name, "max_depth", std::to_string(wanted.max_depth));
    // Blank means automatic; the key is only written once it has been set.
    if (text && (wanted.max_connections || settings->max_connections)) {
        text = servers::set_value(*text, wanted.name, "max_connections",
                                  wanted.max_connections ? std::to_string(*wanted.max_connections) : "");
    }
    if (!text) return undo(text.error());
    const RcloneRemote remote = server_conf::to_remote(settings->rclone_remote, next, env.known_hosts);
    // The lookup as the next run resolves it, the guard's included (the store as just saved).
    const auto resolved = resolve(*settings, next, remote, passwords::load(env.passwords));
    const std::string described = servers::replace_lead_in(
        *text, wanted.name, lead_in(wanted.name, describe_remote(remote, resolved.search, login_said(next))));
    if (auto written = write_grab_conf(env, described); !written) return undo(written.error());

    std::vector<std::string> notes;
    if (!store_note.empty()) notes.push_back(std::move(store_note));
    if (current && moved) {
        if (current->host != next.host || current->port != next.port) {
            Env after = env;
            after.grab_text = described;
            if (auto why = forget_unused_keys(after, current->host, current->port, wanted.name); !why.empty()) {
                notes.push_back(std::move(why));
            }
        }
        if (auto why = forget_learned(env, wanted.name); !why.empty()) notes.push_back(std::move(why));
    }
    return util::join(notes, "; ");
}

std::expected<void, std::string> trust_server(const Env& loaded, std::string_view name, const HostKeys& keys) {
    auto locked = lock_current(loaded);
    if (!locked) return util::fail(locked.error());
    const Env& env = locked->env;
    if (find_server(env.cfg, name) == nullptr) return util::failf("no server called {} in grab.conf", name);
    auto c = connection_in(env.grab_text, name);
    if (!c) return util::fail(c.error());
    if (!*c) {
        return util::failf("[{}] has no connection details in grab.conf: fill them in with Edit (grab server edit {})",
                           name, name);
    }
    return pin_host_keys(env.known_hosts, (*c)->host, (*c)->port, keys.lines);
}

std::expected<std::string, std::string> remove_server(const Env& loaded, std::string_view name) {
    auto locked = lock_current(loaded);
    if (!locked) return util::fail(locked.error());
    const Env& env = locked->env;
    if (find_server(env.cfg, name) == nullptr) return util::failf("no server called {} in grab.conf", name);
    const auto read = connection_in(env.grab_text, name);
    const std::optional<server_conf::Connection> c = read ? *read : std::nullopt; // its address, for its keys
    std::expected<std::string, std::string> text = servers::remove_section(env.grab_text, name);
    if (env.cfg.default_remote == name) {
        std::string next;
        for (const auto& r : env.cfg.remotes) {
            if (r.name != name) {
                next = r.name;
                break;
            }
        }
        text = servers::set_value(*text, "grab", "default_remote", next);
    }
    if (!text) return util::fail(text.error());
    if (auto written = write_grab_conf(env, *text); !written) return util::fail(written.error());

    // The server is gone from grab either way; what couldn't be cleaned up is said.
    const std::string server(name);
    std::vector<std::string> notes;
    if (auto store = load_store(env, name); !store) {
        notes.push_back("its saved password could not be removed: " + store.error());
    } else {
        if (!store->note.empty()) notes.push_back(std::move(store->note));
        if (store->store.erase(server) > 0) {
            if (auto saved = passwords::save(env.passwords, store->store); !saved) {
                notes.push_back("its saved password could not be removed: " + saved.error());
            }
        }
    }
    if (c) {
        Env after = env;
        after.grab_text = *text;
        if (auto why = forget_unused_keys(after, c->host, c->port, name); !why.empty()) notes.push_back(std::move(why));
    }
    std::error_code ec;
    if (std::filesystem::remove(file_list::path_for(file_list::dir_for(env.grab_conf), server), ec); ec) {
        notes.push_back(std::format("its file list could not be removed: {}", ec.message()));
    }
    if (auto why = forget_learned(env, server); !why.empty()) notes.push_back(std::move(why));
    return util::join(notes, "; ");
}

std::expected<void, std::string> set_default(const Env& loaded, std::string_view name) {
    auto locked = lock_current(loaded);
    if (!locked) return util::fail(locked.error());
    const Env& env = locked->env;
    if (find_server(env.cfg, name) == nullptr) return util::failf("no server called {} in grab.conf", name);
    auto text = servers::set_value(env.grab_text, "grab", "default_remote", name);
    if (!text) return util::fail(text.error());
    return write_grab_conf(env, *text);
}

std::expected<void, std::string> describe_servers(const Env& loaded) {
    auto locked = lock_current(loaded);
    if (!locked) return util::fail(locked.error());
    const Env& env = locked->env;
    const auto store = passwords::load(env.passwords);
    std::string text = env.grab_text;
    for (const auto& s : env.cfg.remotes) {
        const auto c = connection_in(text, s.name);
        if (!c || !*c) continue; // no details yet: the comment stays as it is
        const RcloneRemote remote = server_conf::to_remote(s.rclone_remote, **c, env.known_hosts);
        const auto resolved = resolve(s, **c, remote, store);
        text = servers::replace_lead_in(text, s.name,
                                        lead_in(s.name, describe_remote(remote, resolved.search, login_said(**c))));
    }
    return write_grab_conf(env, text);
}

// ---- texts ---------------------------------------------------------------------------------

std::string login_text(const ServerInfo& s) {
    using servers::Auth;
    if (s.auth == Auth::agent) return "ssh-agent";
    if (!s.secret_saved) {
        const char* how = s.auth == Auth::key ? "key file" : s.auth == Auth::stored_key ? "key in grab's store" : "password";
        return std::format("{} (grab's saved passwords can't be read: {})", how, s.store_error);
    }
    switch (s.auth) {
    case Auth::key: return *s.secret_saved ? "key file, passphrase saved" : "key file";
    case Auth::stored_key:
        return *s.secret_saved ? std::string("key in grab's store")
                               : std::format("no key in grab's store (grab server edit {})", s.name);
    default: break;
    }
    return *s.secret_saved ? std::string("password saved") : std::format("no password saved (grab server edit {})", s.name);
}

std::string host_key_text(const ServerInfo& s) {
    return s.host_key_pinned ? std::string("host key pinned")
                             : std::format("host key not trusted (grab server trust {})", s.name);
}

std::string route_text(const ServerInfo& s) {
    if (s.proxy.kind == proxy::Kind::none) return {};
    if (s.proxy_from == guard::ProxyFrom::kept) {
        return std::format("through {} (kept in grab's store: from rclone, or a proxy line since removed; grab server "
                           "edit {} writes it into grab.conf)",
                           proxy::said(s.proxy), s.name);
    }
    return std::format("through {} (in grab.conf)", proxy::said(s.proxy));
}

namespace {

// The keys known_hosts text `text` pins for host:port, each as "type key" (markers and comments
// aside, as server_conf::pinned reads them).
std::vector<std::string> pinned_keys(std::string_view host, int port, std::string_view text) {
    std::vector<std::string> out;
    for (const auto& raw : util::split(text, '\n')) {
        const auto line = util::trim(raw);
        if (line.empty() || line.front() == '#' || line.front() == '@') continue;
        const auto names_end = line.find_first_of(" \t");
        if (names_end == std::string_view::npos) continue;
        const auto names = util::split(line.substr(0, names_end), ',');
        const auto names_it = [&](const std::string& n) { return servers::names_host(n, host, port); };
        if (std::ranges::none_of(names, names_it)) continue;
        const auto rest = util::trim(line.substr(names_end));
        const auto type_end = rest.find_first_of(" \t");
        if (type_end == std::string_view::npos) continue;
        const auto key = util::trim(rest.substr(type_end));
        out.push_back(std::format("{} {}", rest.substr(0, type_end), key.substr(0, key.find_first_of(" \t"))));
    }
    return out;
}

} // namespace

std::string fetch_note(const std::filesystem::path& known_hosts, std::string_view host, int port, const HostKeys& keys,
                       const proxy::Proxy& through) {
    if (through.kind == proxy::Kind::none) return {};
    const std::string text = util::read_file_if_exists(known_hosts).value_or(std::nullopt).value_or("");
    const auto pinned = pinned_keys(host, port, text);
    // A fetched line is "name type key" (ssh::known_host_line).
    const bool known = std::ranges::all_of(keys.lines, [&](const std::string& line) {
        const auto read = pinned_keys(host, port, line);
        return !read.empty() && std::ranges::contains(pinned, read.front());
    });
    if (known) return {};
    return std::format("Read through {}. A proxy can show its own key here; if you can, compare these fingerprints "
                       "with the server's owner.",
                       proxy::said(through));
}

std::string import_banner(const std::vector<std::string>& names, const std::vector<ServerInfo>& servers,
                          const std::vector<std::string>& notes) {
    std::vector<std::string> moved;
    std::vector<std::string> lines;
    std::vector<std::string> trust; // servers a line already tells to Trust
    std::vector<std::string> typed; // and to enter their password
    bool secrets = false;           // one of them has a password, passphrase or key in grab's store
    for (const auto& name : names) {
        const auto s = std::ranges::find(servers, name, &ServerInfo::name);
        if (s == servers.end()) continue; // removed since
        moved.push_back(name);
        secrets = secrets || s->secret_saved == true;
        if (!s->error.empty()) {
            lines.push_back(s->error);
            continue;
        }
        if (!s->host_key_pinned) {
            trust.push_back(name);
            lines.push_back(std::format("{}: its host key isn't trusted yet: use Trust in Settings, which shows its "
                                        "fingerprints to compare.",
                                        name));
        }
        if (s->auth == servers::Auth::password && s->secret_saved == false) {
            typed.push_back(name);
            lines.push_back(std::format("{}: no password is saved for it: enter it with Edit.", name));
        }
    }
    for (const auto& note : notes) {
        // "[server] text"
        const auto close = note.find("] ");
        const std::string server = note.starts_with('[') && close != std::string::npos ? note.substr(1, close - 1) : "";
        const bool told_trust = std::ranges::contains(trust, server) && note.find("Trust") != std::string::npos;
        const bool told_password = std::ranges::contains(typed, server) && note.find("password") != std::string::npos;
        if (!told_trust && !told_password) lines.push_back(note);
    }
    if (moved.empty() && lines.empty()) return {};
    std::vector<std::string> out;
    if (!moved.empty()) {
        out.push_back(std::format("Servers are kept in grab.conf now: the connection details of {} moved there from "
                                  "rclone.conf.{}",
                                  util::join(moved, ", "),
                                  secrets ? " Their passwords are in grab's own encrypted store." : ""));
    }
    out.insert(out.end(), lines.begin(), lines.end());
    return util::join(out, "\n");
}

namespace {

// What the test learned (ssh::learn_commands) into grab.conf, under the settings lock and from
// the file as it is then: only keys the section still lacks, and only while it is at the login
// that answered, so neither another grab's change nor an edit meanwhile is overwritten.
void save_commands(const Env& env, std::string_view name, const server_conf::Connection& asked,
                   const ssh::ServerCommands& learned) {
    auto lock = lock_settings();
    if (!lock) return;
    auto text = util::read_file(env.grab_conf);
    if (!text) return;
    auto now = connection_in(*text, name);
    if (!now || !*now) return;
    auto& c = **now;
    if (c.host != asked.host || c.port != asked.port || c.user != asked.user) return;
    std::expected<std::string, std::string> changed = *text;
    for (const auto& [key, have, value] : {std::tuple{"shell_type", &c.shell_type, &learned.shell_type},
                                          std::tuple{"md5sum_command", &c.md5sum_command, &learned.md5sum_command},
                                          std::tuple{"sha1sum_command", &c.sha1sum_command, &learned.sha1sum_command}}) {
        if (!changed || *have || !*value) continue;
        // After host, port, user, auth and the like, in server_conf::keys' order.
        const std::span<const std::string_view> keys = server_conf::keys;
        const auto before = keys.first(static_cast<std::size_t>(std::ranges::find(keys, key) - keys.begin()));
        changed = servers::set_value(*changed, name, key, **value, before);
    }
    if (changed && *changed != *text) {
        if (auto written = util::write_file_atomic(env.grab_conf, *changed); !written) {
            // Asked again by the next Test.
        }
    }
}

// The test: a login through grab's own client (the target downloads use) and its home folder
// listed, then the server's shell and sum commands (saved in grab.conf), then the find check on
// that connection, which says how searches go; its answer replaces the one saved in `limits_file`
// (engine::find_memory), so a server changed since is searched the new way by the next command
// (the window drops its pool's answer, App::test_server). The hash check last, which says whether
// downloads can be verified there (phase 5). Nothing is set when stopped.
void test_grab(const Env& env, engine::Context& ctx, const std::filesystem::path& limits_file, const std::stop_token& stop,
               TestResult& result) {
    auto failed = [&](std::string why) {
        result.grab_ok = false;
        result.grab_error = std::move(why);
    };
    auto target = engine::ssh_target(ctx);
    if (!target) return failed(target.error());
    ssh::Pool::Options options;
    options.limit = 1;
    options.idle_timer = false;
    ssh::Pool pool([&target] { return ssh::open_session(*target); }, options);
    try {
        auto lease = pool.acquire(stop, ssh::interactive_retry);
        if (!lease) return;
        // Its login folder listed (phase 7 step 8).
        if (auto listed = sftp::list((*lease)->sftp(), "."); !listed) {
            return failed("its home folder can't be listed: " + sftp::describe(listed.error()));
        }
    } catch (const ssh::Error& e) {
        return failed(e.what());
    } catch (const sftp::Broken& e) {
        return failed(e.what());
    }
    // Once per server (and again after its login moves): verification needn't ask on every run.
    const ssh::ServerCommands known{.shell_type = ctx.connection.shell_type,
                                    .md5sum_command = ctx.connection.md5sum_command,
                                    .sha1sum_command = ctx.connection.sha1sum_command};
    const auto learned = ssh::learn_commands(pool, known, stop, ssh::interactive_retry);
    if (stop.stop_requested()) return;
    if (learned != known) {
        save_commands(env, ctx.settings.name, ctx.connection, learned);
        ctx.connection.shell_type = learned.shell_type;
        ctx.connection.md5sum_command = learned.md5sum_command;
        ctx.connection.sha1sum_command = learned.sha1sum_command;
        ctx.remote.shell_type = learned.shell_type;
        ctx.remote.md5sum_command = learned.md5sum_command;
    }
    {
        // Asked whatever was saved; a save that fails is only asked again by the next search.
        // gui.json's old limits, kept if this answer makes limits.json (review G195).
        auto gui = read_gui_state(limits_file.parent_path() / "gui.json");
        auto old_limits = gui && *gui ? std::move((*gui)->server_limits) : std::map<std::string, int>{};
        auto memory = engine::find_memory(limits_file, *target, {}, std::move(old_limits));
        memory.load = {};
        auto way = ssh::choose_lookup(pool, ctx.grab_method, ctx.settings.name, stop, ssh::interactive_retry, memory);
        if (stop.stop_requested()) return;
        if (!way) return failed(way.error());
        result.grab_search = engine::lookup_text(ctx, *way);
    }
    // Whether verification can run: grab.conf's keys, then the server's md5sum.
    if (auto blocked = ssh::hash_blocked(ctx.remote)) {
        result.checksums = "verify can't run: " + blocked->why;
    } else {
        const auto check = ssh::hash_check(pool, stop, ssh::interactive_retry);
        if (!check) return; // stopped
        // The login worked a moment ago: one failing now is the test's failure, not the check's.
        if (!check->works && check->failure == ssh::HashFailure::login) return failed(check->answer);
        result.checksums = check->works ? "checksums: md5sum" : "verify can't run: " + ssh::hash_check_failed(*check).why;
    }
    result.grab_ok = true;
}

} // namespace

std::expected<TestResult, std::string> test_connection(const Env& env, std::string_view name,
                                                       std::stop_token stop) {
    const auto* s = find_server(env.cfg, name);
    if (s == nullptr) return util::failf("no server called {} in grab.conf", name);
    auto c = connection_in(env.grab_text, name);
    if (!c) return util::fail(c.error());
    if (!*c) {
        return util::failf("[{}] has no connection details in grab.conf: fill them in with Edit (grab server edit {})",
                           name, name);
    }

    engine::Context ctx = engine::make_context(env.cfg, *s, std::move(**c), env.grab_conf);
    TestResult result;
    // A server grab doesn't connect to says why, and nothing is tried (phase 9 steps 6 and 7).
    if (const auto blocked = engine::grab_blockers(ctx); !blocked.empty()) {
        result.unusable = guard::text(name, blocked);
        return result;
    }
    if (auto reach = engine::connect_settings(ctx.settings); !reach) {
        result.unusable = reach.error();
        return result;
    }
    // A server grab has no key for is refused before anything dials (Main's fail2ban counts each
    // handshake cut short).
    if (auto pinned = engine::check_pinned(ctx); !pinned) {
        result.grab_ok = false;
        result.grab_error = pinned.error();
        return result;
    }
    test_grab(env, ctx, learned_limits_path(env.grab_conf), stop, result);
    if (stop.stop_requested()) return util::fail("cancelled");
    return result;
}

std::vector<std::string> test_lines(const TestResult& t) {
    if (!t.unusable.empty()) return {"not usable: " + t.unusable};
    if (!t.grab_ok) return {};
    if (!*t.grab_ok) return {"grab: FAILED: " + t.grab_error};
    std::string how = "grab: OK, home folder listed";
    if (!t.grab_search.empty()) how += ", searches " + t.grab_search;
    if (!t.checksums.empty()) how += ", " + t.checksums;
    return {how};
}

} // namespace grab::server_ops
