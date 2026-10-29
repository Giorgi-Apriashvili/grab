#include "server_ops.hpp"

#include "conn_budget.hpp"
#include "engine.hpp"
#include "file_list.hpp"
#include "gui_state.hpp"
#include "ini.hpp"
#include "passwords.hpp"
#include "process.hpp"
#include "rclone_import.hpp"
#include "remote.hpp"
#include "server_conf.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_find.hpp"
#include "ssh_hash.hpp"
#include "ssh_pool.hpp"
#include "util.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <format>
#include <functional>
#include <map>
#include <span>
#include <system_error>
#include <tuple>
#include <utility>

namespace grab::server_ops {

namespace {

std::string first_line(std::string_view text) {
    text = util::trim(text);
    const auto nl = text.find('\n');
    return std::string(util::trim(nl == std::string_view::npos ? text : text.substr(0, nl)));
}

proc::RunOptions quiet(std::stop_token stop = {}) {
    proc::RunOptions o;
    o.detached = true;
    o.stop = std::move(stop);
    return o;
}

bool ok(const std::expected<proc::CaptureResult, std::string>& run) { return run && run->exit_code == 0; }

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
                           "moved aside to {}",
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

} // namespace

std::string clean_rclone_error(std::string_view err) {
    std::string line = first_line(err);
    std::string_view v = line;
    auto digit = [](char ch) { return ch >= '0' && ch <= '9'; };
    if (v.size() > 20 && digit(v[0]) && v[4] == '/' && v[7] == '/' && v[10] == ' ' && v[13] == ':' && v[19] == ' ') {
        v.remove_prefix(20);
    }
    for (std::string_view level : {"CRITICAL: ", "ERROR : ", "NOTICE: ", "INFO  : "}) {
        if (v.starts_with(level)) {
            v.remove_prefix(level.size());
            break;
        }
    }
    return std::string(v);
}

std::expected<Env, std::string> load_env(const std::filesystem::path& grab_conf,
                                         const std::filesystem::path& exe_dir) {
    Env env;
    env.grab_conf = grab_conf;
    if (auto applied = rclone_import::apply(rclone_import::files_for(grab_conf)); !applied) {
        env.import_notes.push_back(applied.error());
    } else {
        for (const auto& server : applied->imported) env.imported.push_back(server.name);
        for (const auto& note : applied->notes) env.import_notes.push_back(std::format("[{}] {}", note.server, note.text));
    }
    if (auto exists = read_grab_conf(env); !exists) return util::fail(exists.error());
    env.known_hosts = grab_known_hosts_path();
    env.passwords = passwords::path_in(state_dir(grab_conf));
    env.rclone = resolve_rclone_exe(env.cfg, exe_dir);
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

std::vector<ServerInfo> list_servers(const Env& env) {
    std::vector<ServerInfo> out;
    // Read when asked (G209): pinned is what the file holds now.
    const std::string known = util::read_file_if_exists(env.known_hosts).value_or(std::nullopt).value_or("");
    for (const auto& s : env.cfg.remotes) {
        ServerInfo info;
        info.name = s.name;
        info.rclone_remote = s.rclone_remote;
        info.is_default = is_default_server(env.cfg, s.name);
        for (const auto& r : s.search_roots) info.search_roots.push_back(r.empty() ? "~" : r);
        info.max_depth = s.max_depth;
        info.max_connections = s.max_connections;
        auto c = connection_in(env.grab_text, s.name);
        if (!c) {
            info.error = c.error();
        } else if (!*c) {
            info.error = std::format("[{}] has no connection details (host, port, user, auth) in grab.conf: fill them in "
                                     "with Edit",
                                     s.name);
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
            info.engine = effective_engine(s);
            info.search = std::string(search_method(s, remote, info.engine));
            info.host_key_pinned = server_conf::pinned(known, conn.host, conn.port);
            info.default_connections = budget::default_budget(conn.host);
        }
        out.push_back(std::move(info));
    }
    return out;
}

// ---- host keys -----------------------------------------------------------------------------

std::expected<HostKeys, std::string> scan_host_keys(const std::string& host, int port, std::stop_token stop) {
    auto scanned = ssh::scan_host_keys(host, port, std::chrono::seconds(10), std::move(stop));
    if (!scanned) return util::fail(scanned.error());
    HostKeys keys;
    for (const auto& key : *scanned) {
        keys.lines.push_back(ssh::known_host_line(host, port, key));
        keys.fingerprints.push_back(servers::Fingerprint{.hash = key.fingerprint, .type = servers::key_label(key.type)});
    }
    return keys;
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
    const server_conf::Connection c = connection_of(s);

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

bool edit_needs_host_key(const ServerInfo& current, const servers::NewServer& wanted) {
    return !current.host_key_pinned || current.host != wanted.host || current.port != wanted.port;
}

bool edit_needs_password(const ServerInfo& current, const servers::NewServer& wanted) {
    if (wanted.auth != servers::Auth::password) return false;
    // As the store's binding compares them: exactly.
    return current.auth != servers::Auth::password || current.host != wanted.host || current.port != wanted.port ||
           current.user != wanted.user;
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

    server_conf::Connection next = current.value_or(server_conf::Connection{});
    next.host = wanted.host;
    next.port = wanted.port;
    next.user = wanted.user;
    next.auth = wanted.auth;
    if (wanted.auth == servers::Auth::stored_key && (!current || current->auth != servers::Auth::stored_key)) {
        return util::fail("a key kept in grab's store can only be imported: choose another way to log in");
    }
    // The key file of `key`; beside `agent` (which agent key) the one given, else the one it had.
    const bool agent_kept = wanted.auth == servers::Auth::agent && wanted.key_file.empty() && current &&
                            current->auth == servers::Auth::agent;
    if (!agent_kept) next.key_file = connection_of(wanted).key_file;
    const bool moved = !current || current->host != next.host || current->port != next.port || current->user != next.user;
    const bool method = !current || current->auth != next.auth;
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

    // The store, only when the login method, the secret or the login changes (G85).
    std::optional<passwords::Store> before;
    passwords::Store store;
    std::string store_note;
    const bool secret_kind = wanted.auth == servers::Auth::password || wanted.auth == servers::Auth::key;
    if (method || moved || (typed && secret_kind)) {
        auto loaded_store = load_store(env, wanted.name);
        if (!loaded_store) return util::fail(loaded_store.error());
        store_note = std::move(loaded_store->note);
        before = loaded_store->store;
        store = std::move(loaded_store->store);
        const passwords::Binding login{.user = next.user, .host = next.host, .port = next.port};
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
            // imported key and rclone extras belong to this server wherever it moved.
            if (moved) values.erase(std::string(passwords::password));
            for (auto& [key, saved] : values) saved.binding = login;
            if (values.empty()) store.erase(it);
        }
        if (typed && secret_kind) {
            const auto key = wanted.auth == servers::Auth::password ? passwords::password : passwords::passphrase;
            store[wanted.name][std::string(key)] = passwords::Secret{.binding = login, .value = *secret};
        }
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
    const std::string described = servers::replace_lead_in(
        *text, wanted.name, std::format("# rclone remote [{}]: {}", remote.name, describe_remote(remote, *settings)));
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
    if (!*c) return util::failf("[{}] has no connection details in grab.conf: fill them in with Edit", name);
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

// grab's own client's part of a test: a login through it (the target downloads use) and its home
// folder listed, then what rclone would ask the server (saved in grab.conf), then on engine = grab
// the find check on that connection, which says how searches go; its answer replaces the one
// saved in `limits_file` (engine::find_memory), so a server changed since is searched the new way
// by the next command (the window drops its pool's answer, App::test_server). On every server the
// hash check last, which says whether downloads can be verified there (phase 5). Nothing is set
// when stopped.
void test_grab(const Env& env, engine::Context& ctx, const std::filesystem::path& limits_file, const std::stop_token& stop,
               TestResult& result) {
    auto failed = [&](std::string why) {
        result.grab_ok = false;
        result.grab_error = std::move(why);
    };
    const bool grab_engine = ctx.engine == Engine::grab;
    result.grab_engine = grab_engine;
    auto target = engine::ssh_target(ctx);
    if (!target) return failed(target.error());
    ssh::Pool::Options options;
    options.limit = 1;
    options.idle_timer = false;
    ssh::Pool pool([&target] { return ssh::open_session(*target); }, options);
    try {
        auto lease = pool.acquire(stop, ssh::interactive_retry);
        if (!lease) return;
        // Its login folder listed on every server (phase 7 step 8): on engine = rclone a failure
        // here is a warning, as for the login.
        if (auto listed = sftp::list((*lease)->sftp(), "."); !listed) {
            return failed("its home folder can't be listed: " + sftp::describe(listed.error()));
        }
    } catch (const ssh::Error& e) {
        return failed(e.what());
    } catch (const sftp::Broken& e) {
        return failed(e.what());
    }
    // Once per server (and again after its login moves): rclone then needn't ask on every run.
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
    if (grab_engine) {
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
    if (!*c) return util::failf("[{}] has no connection details in grab.conf: fill them in with Edit", name);

    GrabConfig cfg = env.cfg;
    cfg.rclone = env.rclone; // as load_context resolves it
    engine::Context ctx = engine::make_context(std::move(cfg), *s, std::move(**c), env.grab_conf);
    TestResult result;
    const bool on_rclone = ctx.engine != Engine::grab;
    const bool with_ssh = on_rclone && ctx.method == FindMethod::ssh;
    // A server grab has no key for is refused before anything dials (Main's fail2ban counts each
    // handshake cut short).
    if (auto pinned = engine::check_pinned(ctx); !pinned) {
        result.grab_engine = !on_rclone;
        result.grab_ok = false;
        result.grab_error = pinned.error();
        if (on_rclone) {
            result.rclone_ok = false;
            result.rclone_error = pinned.error();
        }
        if (with_ssh) {
            result.ssh_ok = false;
            result.ssh_error = pinned.error();
        }
        return result;
    }

    // rclone only where it still downloads: on engine = grab nothing a test can show goes through it
    // (previews and Play don't either since phase 6), and its listing costs a login (1-3.5 s on a
    // Storage Box).
    if (on_rclone) {
        auto probe = engine::rclone_env(ctx).and_then([&](proc::Environment server) {
            auto run = quiet(stop);
            run.environment = std::move(server);
            return proc::run_capture(servers::probe_argv(env.rclone, s->rclone_remote), run);
        });
        if (stop.stop_requested()) return util::fail("cancelled");
        result.rclone_ok = ok(probe);
        if (!*result.rclone_ok) result.rclone_error = probe ? clean_rclone_error(probe->err) : probe.error();
    }

    if (with_ssh) {
        std::vector<std::string> options{"-o", "BatchMode=yes", "-o", "ConnectTimeout=15"};
        options.insert(options.end(), s->ssh_options.begin(), s->ssh_options.end());
        auto ssh = proc::run_capture(build_ssh_argv(env.cfg.ssh, ctx.remote, options, "echo grab-ok"), quiet(stop));
        if (stop.stop_requested()) return util::fail("cancelled");
        result.ssh_ok = ok(ssh) && ssh->out.find("grab-ok") != std::string::npos;
        if (!*result.ssh_ok) result.ssh_error = ssh ? first_line(ssh->err) : ssh.error();
    }
    // Every server: grab's own login, which downloads and searches use on engine = grab, and
    // previews, Play and verification on any.
    test_grab(env, ctx, learned_limits_path(env.grab_conf), stop, result);
    if (stop.stop_requested()) return util::fail("cancelled");
    return result;
}

} // namespace grab::server_ops
