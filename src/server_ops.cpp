#include "server_ops.hpp"

#include "conn_budget.hpp"
#include "process.hpp"
#include "remote.hpp"
#include "util.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <system_error>

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

// "rclone config update failed: <first line of its stderr>"
std::unexpected<std::string> run_failed(std::string_view what,
                                        const std::expected<proc::CaptureResult, std::string>& run) {
    return util::failf("{} failed: {}", what, run ? clean_rclone_error(run->err) : run.error());
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
    env.rclone_conf = effective_rclone_config(env.cfg);
    return Locked{.lock = std::move(*lock), .env = std::move(env)};
}

// The server's host key lines via real ssh handshakes, one per key type (see
// servers::handshake_argv): the fallback for when ssh-keyscan cannot negotiate.
std::expected<std::vector<std::string>, std::string> handshake_host_key(const std::string& ssh,
                                                                        const std::string& host, int port,
                                                                        const std::stop_token& stop) {
    std::error_code ec;
    std::string last_error;
    std::vector<std::string> lines;
    for (const auto& algorithm : servers::handshake_key_algorithms()) {
        if (stop.stop_requested()) return util::fail("cancelled");
        // A fresh record file each time: once a host is known with one key type, accept-new
        // would not record another.
        const auto record = std::filesystem::temp_directory_path() /
                            util::path_from_utf8(std::format("grab-hostkey-{}-{}-{}.txt", host, port, algorithm));
        std::filesystem::remove(record, ec);
        auto run = proc::run_capture(servers::handshake_argv(ssh, host, port, record, algorithm), quiet(stop));
        auto text = util::read_file(record);
        std::filesystem::remove(record, ec);
        if (!run) return util::fail(run.error());
        if (auto why = first_line(run->err); !why.empty()) last_error = why;
        if (text) {
            for (auto& l : servers::parse_keyscan(*text)) {
                if (std::ranges::find(lines, l) == lines.end()) lines.push_back(std::move(l));
            }
        }
    }
    if (lines.empty()) {
        return util::failf("could not read a host key from {}:{}{}", host, port,
                           last_error.empty() ? "" : ": " + last_error);
    }
    return lines;
}

bool truthy(std::string_view v) {
    const auto t = util::to_lower(util::trim(v));
    return t == "true" || t == "1" || t == "yes" || t == "on";
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
    auto exists = read_grab_conf(env);
    if (!exists) return util::fail(exists.error());
    if (*exists) env.imported = migrate_rclone_remotes(env.cfg).imported;
    env.rclone_conf = effective_rclone_config(env.cfg);
    env.known_hosts = grab_known_hosts_path();
    env.rclone = resolve_rclone_exe(env.cfg, exe_dir);
    if (!env.cfg.rclone_config) {
        if (auto ready = secrets::prepare(env.rclone_conf); !ready) env.secrets_error = ready.error();
    }
    return env;
}

void apply_auth(secrets::Store& store, const std::string& remote, servers::Auth auth,
                const std::optional<std::string>& obscured) {
    auto& values = store[remote];
    switch (auth) {
    case servers::Auth::password:
        values.erase("key_file_pass");
        values.erase("key_pem");
        if (obscured) values["pass"] = *obscured;
        break;
    case servers::Auth::key_file:
        values.erase("pass");
        values.erase("key_pem");
        if (obscured) values["key_file_pass"] = *obscured;
        break;
    case servers::Auth::agent:
        values.clear();
        break;
    }
    if (values.empty()) store.erase(remote);
}

namespace {

// Changes the encrypted store of grab's own rclone.conf and hands the result to rclone. A store
// that can't be decrypted starts over, the old file set aside (secrets::load_for_change); the
// note returned then says so, and which other servers (but `leaving`, one being removed) need
// their password again (review G211). Empty otherwise.
std::expected<std::string, std::string> change_secrets(const Env& env, const std::function<void(secrets::Store&)>& change,
                                                       std::string_view leaving = {}) {
    auto lock = lock_settings();
    if (!lock) return util::fail(lock.error());
    const auto file = secrets::path_beside(env.rclone_conf);
    auto loaded = secrets::load_for_change(file);
    if (!loaded) return util::fail(loaded.error());
    change(loaded->store);
    if (auto saved = secrets::save(file, loaded->store); !saved) return util::fail(saved.error());
    secrets::export_to_env(loaded->store);
    if (!loaded->set_aside) return std::string{};
    std::vector<std::string> again;
    for (const auto& s : list_servers(env)) {
        if (s.error.empty() && s.auth == servers::Auth::password && s.rclone_remote != leaving &&
            !loaded->store.contains(s.rclone_remote)) {
            again.push_back(s.name);
        }
    }
    std::string note = std::format("the saved passwords couldn't be read and were moved to {}",
                                   util::path_to_utf8(*loaded->set_aside));
    if (!again.empty()) note += std::format("; enter the password of {} again (Edit)", util::join(again, ", "));
    return note;
}

} // namespace

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
    for (const auto& s : env.cfg.remotes) {
        ServerInfo info;
        info.name = s.name;
        info.rclone_remote = s.rclone_remote;
        info.is_default = is_default_server(env.cfg, s.name);
        for (const auto& r : s.search_roots) info.search_roots.push_back(r.empty() ? "~" : r);
        info.max_depth = s.max_depth;
        info.max_connections = s.max_connections;
        auto remote = load_rclone_remote(env.rclone_conf, s.rclone_remote);
        if (!remote) {
            info.error = remote.error();
        } else {
            info.host = remote->host;
            info.user = remote->user;
            info.port = remote->port;
            info.auth = remote->key_use_agent ? servers::Auth::agent
                        : remote->key_file    ? servers::Auth::key_file
                                              : servers::Auth::password;
            info.key_file = remote->key_file.value_or("");
            info.ssh_search = resolve_find_method(s, *remote) == FindMethod::ssh;
            info.host_key_pinned = remote->known_hosts_file.has_value();
            info.default_connections = budget::default_budget(remote->host);
        }
        out.push_back(std::move(info));
    }
    return out;
}

// ---- host keys -----------------------------------------------------------------------------

std::expected<HostKeys, std::string> scan_host_keys(const std::string& ssh, const std::string& host, int port,
                                                    std::stop_token stop) {
    HostKeys keys;
    auto scan = proc::run_capture(servers::keyscan_argv(host, port), quiet(stop));
    if (stop.stop_requested()) return util::fail("cancelled");
    if (scan) keys.lines = servers::parse_keyscan(scan->out);
    if (keys.lines.empty()) {
        auto shake = handshake_host_key(ssh, host, port, stop);
        if (!shake) return util::fail(shake.error());
        keys.lines = std::move(*shake);
    }
    auto opts = quiet(stop);
    opts.input = util::join(keys.lines, "\n") + "\n";
    if (auto fp = proc::run_capture(std::vector<std::string>{"ssh-keygen", "-lf", "-"}, opts)) {
        keys.fingerprints = servers::parse_fingerprints(fp->out);
    }
    return keys;
}

std::expected<void, std::string> pin_host_keys(const std::filesystem::path& known_hosts,
                                               const std::vector<std::string>& lines) {
    auto lock = lock_settings();
    if (!lock) return util::fail(lock.error());
    auto existing = util::read_file_if_exists(known_hosts);
    // One that can't be read is never replaced by the new lines alone: every other server's
    // pins would go with it (review G89).
    if (!existing) return util::fail(existing.error());
    return util::write_file_atomic(known_hosts, servers::merge_known_hosts(existing->value_or(""), lines));
}

std::expected<ini::Document, std::string> read_rclone_conf(const Env& env) {
    auto text = util::read_file_if_exists(env.rclone_conf);
    if (!text) return util::fail(text.error());
    auto doc = ini::parse(text->value_or(""));
    if (!doc) return util::failf("{}: {}", util::path_to_utf8(env.rclone_conf), doc.error());
    return std::move(*doc);
}

std::optional<std::string> name_taken(const Env& env, const ini::Document& rclone, const std::string& name) {
    if (find_server(env.cfg, name) != nullptr) return std::format("grab.conf already has a server called {}", name);
    if (rclone.find(name) != nullptr) return std::format("grab's rclone.conf already has a remote called {}", name);
    return std::nullopt;
}

std::expected<std::string, std::string> obscure(const Env& env, const std::string& secret) {
    auto opts = quiet();
    opts.input = secret; // via stdin: the plaintext never appears on a command line
    auto run = proc::run_capture(servers::obscure_argv(env.rclone), opts);
    if (!ok(run)) return run_failed("rclone obscure", run);
    return first_line(run->out);
}

// ---- changes -------------------------------------------------------------------------------

std::expected<std::string, std::string> add_server(const Env& loaded, const servers::NewServer& s,
                                            const std::optional<std::string>& obscured_secret,
                                            const HostKeys& keys) {
    auto locked = lock_current(loaded);
    if (!locked) return util::fail(locked.error());
    const Env& env = locked->env;
    // The name once more, now that no other change can come in between; `rclone config create`
    // would replace a remote of that name (review G217).
    auto rclone_doc = read_rclone_conf(env);
    if (!rclone_doc) return util::fail(rclone_doc.error());
    if (auto why = name_taken(env, *rclone_doc, s.name)) return util::fail(*why);

    if (auto pinned = pin_host_keys(env.known_hosts, keys.lines); !pinned) return util::fail(pinned.error());
    std::error_code ec;
    std::filesystem::create_directories(env.rclone_conf.parent_path(), ec);
    // grab's own rclone.conf gets no secret (nor does the command line): the store keeps it.
    const bool own = !env.cfg.rclone_config;
    auto created = proc::run_capture(servers::create_argv(env.rclone, env.rclone_conf, s,
                                                          own ? std::nullopt : obscured_secret, env.known_hosts),
                                     quiet());
    if (!ok(created)) return run_failed("rclone config create", created);

    // From here a failure takes the remote out again, with its secret, so that the name can be
    // added once more (review G218).
    bool secret_saved = false;
    std::string note;
    auto undo = [&](std::string why) {
        if (secret_saved) {
            if (auto forgot = change_secrets(env, [&](secrets::Store& st) { st.erase(s.name); }); !forgot) {
                why += std::format("; its saved password could not be removed again: {}", forgot.error());
            }
        }
        auto deleted = proc::run_capture(servers::delete_argv(env.rclone, env.rclone_conf, s.name), quiet());
        if (!ok(deleted)) {
            why += std::format("; rclone config delete {} failed too: {}", s.name,
                               deleted ? clean_rclone_error(deleted->err) : deleted.error());
        }
        return util::fail(std::move(why));
    };
    if (own) {
        auto kept = change_secrets(env, [&](secrets::Store& st) { apply_auth(st, s.name, s.auth, obscured_secret); });
        if (!kept) return undo(kept.error());
        secret_saved = true;
        note = std::move(*kept);
    }

    const RcloneRemote remote = servers::to_remote(s, env.known_hosts);
    std::string text = servers::append_section(env.grab_text, remote_section(remote, s.search_roots, s.max_depth));
    if (s.max_connections) {
        text = servers::set_value(text, s.name, "max_connections", std::to_string(*s.max_connections));
    }
    if (!env.cfg.default_remote || env.cfg.remotes.empty()) {
        text = servers::set_value(text, "grab", "default_remote", s.name);
    }
    if (auto written = write_grab_conf(env, text); !written) return undo(written.error());
    return note;
}

std::vector<std::pair<std::string, std::string>>
rclone_edit_updates(const ini::Section& current, const servers::NewServer& wanted,
                    const std::optional<std::string>& obscured_secret) {
    std::vector<std::pair<std::string, std::string>> out;
    auto cur = [&](std::string_view key) { return std::string(util::trim(current.get(key).value_or(""))); };
    auto set = [&](const std::string& key, const std::string& value) {
        if (cur(key) != value) out.emplace_back(key, value);
    };
    auto clear = [&](const std::string& key) {
        if (!cur(key).empty()) out.emplace_back(key, "");
    };

    set("host", wanted.host);
    const std::string port = std::to_string(wanted.port);
    if ((cur("port").empty() ? std::string("22") : cur("port")) != port) out.emplace_back("port", port);
    set("user", wanted.user);

    const bool agent_on = truthy(cur("key_use_agent"));
    switch (wanted.auth) {
    case servers::Auth::password:
        if (obscured_secret) out.emplace_back("pass", *obscured_secret);
        clear("key_file");
        clear("key_file_pass");
        if (agent_on) out.emplace_back("key_use_agent", "false");
        break;
    case servers::Auth::key_file:
        set("key_file", wanted.key_file);
        if (obscured_secret) out.emplace_back("key_file_pass", *obscured_secret);
        clear("pass");
        if (agent_on) out.emplace_back("key_use_agent", "false");
        break;
    case servers::Auth::agent:
        if (!agent_on) out.emplace_back("key_use_agent", "true");
        clear("pass");
        clear("key_file");
        clear("key_file_pass");
        break;
    }
    return out;
}

bool edit_needs_host_key(const ServerInfo& current, const servers::NewServer& wanted) {
    return !current.host_key_pinned || current.host != wanted.host || current.port != wanted.port;
}

bool edit_needs_password(const ServerInfo& current, const servers::NewServer& wanted) {
    if (wanted.auth != servers::Auth::password) return false;
    return current.auth != servers::Auth::password || util::to_lower(current.host) != util::to_lower(wanted.host) ||
           current.user != wanted.user;
}

std::expected<std::string, std::string> update_server(const Env& loaded, const servers::NewServer& wanted,
                                               const std::optional<std::string>& obscured_secret,
                                               const std::optional<HostKeys>& keys) {
    auto locked = lock_current(loaded);
    if (!locked) return util::fail(locked.error());
    const Env& env = locked->env;
    const auto* settings = find_server(env.cfg, wanted.name);
    if (settings == nullptr) return util::failf("no server called {} in grab.conf", wanted.name);
    auto rclone_text = util::read_file(env.rclone_conf);
    if (!rclone_text) return util::fail(rclone_text.error());
    auto doc = ini::parse(*rclone_text);
    if (!doc) return util::failf("{}: {}", util::path_to_utf8(env.rclone_conf), doc.error());
    const auto* section = doc->find(settings->rclone_remote);
    if (section == nullptr) {
        return util::failf("{} has no remote called {}", util::path_to_utf8(env.rclone_conf), settings->rclone_remote);
    }

    if (wanted.auth == servers::Auth::password && !obscured_secret) {
        // As edit_needs_password: the saved password stays with the server it was typed for.
        const auto cur = [&](std::string_view key) { return std::string(util::trim(section->get(key).value_or(""))); };
        const bool was_password = !truthy(cur("key_use_agent")) && cur("key_file").empty();
        if (!was_password || util::to_lower(cur("host")) != util::to_lower(wanted.host) || cur("user") != wanted.user) {
            return util::fail("enter the password again: it is only kept for the same host and user");
        }
    }
    const bool own = !env.cfg.rclone_config; // own: the secret goes to the store, not rclone.conf
    auto updates = rclone_edit_updates(*section, wanted, own ? std::nullopt : obscured_secret);
    if (keys) {
        if (auto pinned = pin_host_keys(env.known_hosts, keys->lines); !pinned) return util::fail(pinned.error());
        const std::string known = util::path_to_utf8(env.known_hosts);
        if (section->get("known_hosts_file").value_or("") != known) updates.emplace_back("known_hosts_file", known);
    }
    if (!updates.empty()) {
        auto run = proc::run_capture(servers::update_argv(env.rclone, env.rclone_conf, settings->rclone_remote, updates),
                                     quiet());
        if (!ok(run)) return run_failed("rclone config update", run);
    }
    std::string note;
    if (own) {
        auto kept = change_secrets(env, [&](secrets::Store& st) {
            apply_auth(st, settings->rclone_remote, wanted.auth, obscured_secret);
        });
        if (!kept) return util::fail(kept.error());
        note = std::move(*kept);
    }

    std::string text = servers::set_value(env.grab_text, wanted.name, "search_roots",
                                          search_roots_value(wanted.search_roots));
    text = servers::set_value(text, wanted.name, "max_depth", std::to_string(wanted.max_depth));
    // Blank means automatic; the key is only written once it has been set.
    if (wanted.max_connections || settings->max_connections) {
        text = servers::set_value(text, wanted.name, "max_connections",
                                  wanted.max_connections ? std::to_string(*wanted.max_connections) : "");
    }
    servers::NewServer described = wanted;
    described.name = settings->rclone_remote;
    RcloneRemote remote = servers::to_remote(described, {});
    text = servers::replace_lead_in(text, wanted.name,
                                    std::format("# rclone remote [{}]: {}", remote.name, describe_remote(remote)));
    if (auto written = write_grab_conf(env, text); !written) return util::fail(written.error());
    return note;
}

std::expected<void, std::string> trust_server(const Env& loaded, std::string_view name, const HostKeys& keys) {
    auto locked = lock_current(loaded);
    if (!locked) return util::fail(locked.error());
    const Env& env = locked->env;
    const auto* s = find_server(env.cfg, name);
    if (s == nullptr) return util::failf("no server called {} in grab.conf", name);
    if (auto pinned = pin_host_keys(env.known_hosts, keys.lines); !pinned) return pinned;
    auto updated = proc::run_capture(servers::update_argv(env.rclone, env.rclone_conf, s->rclone_remote,
                                                          "known_hosts_file", util::path_to_utf8(env.known_hosts)),
                                     quiet());
    if (!ok(updated)) return run_failed("rclone config update", updated);
    return {};
}

std::expected<std::string, std::string> remove_server(const Env& loaded, std::string_view name) {
    auto locked = lock_current(loaded);
    if (!locked) return util::fail(locked.error());
    const Env& env = locked->env;
    const auto* s = find_server(env.cfg, name);
    if (s == nullptr) return util::failf("no server called {} in grab.conf", name);
    const std::string rclone_name = s->rclone_remote;
    std::string text = servers::remove_section(env.grab_text, name);
    if (env.cfg.default_remote == name) {
        std::string next;
        for (const auto& r : env.cfg.remotes) {
            if (r.name != name) {
                next = r.name;
                break;
            }
        }
        text = servers::set_value(text, "grab", "default_remote", next);
    }
    if (auto written = write_grab_conf(env, text); !written) return util::fail(written.error());
    // Only grab's own rclone.conf is edited; an explicitly configured shared one is left alone.
    if (env.cfg.rclone_config) return std::string{};
    // The server is gone from grab either way; what couldn't be cleaned up is said.
    std::vector<std::string> notes;
    if (auto forgot = change_secrets(env, [&](secrets::Store& st) { st.erase(rclone_name); }, rclone_name); !forgot) {
        notes.push_back(std::format("its saved password could not be removed: {}", forgot.error()));
    } else if (!forgot->empty()) {
        notes.push_back(std::move(*forgot));
    }
    auto deleted = proc::run_capture(servers::delete_argv(env.rclone, env.rclone_conf, rclone_name), quiet());
    if (!ok(deleted)) {
        notes.push_back(std::format("rclone config delete {} failed: {}", rclone_name,
                                    deleted ? clean_rclone_error(deleted->err) : deleted.error()));
    }
    return util::join(notes, "; ");
}

std::expected<void, std::string> set_default(const Env& loaded, std::string_view name) {
    auto locked = lock_current(loaded);
    if (!locked) return util::fail(locked.error());
    const Env& env = locked->env;
    if (find_server(env.cfg, name) == nullptr) return util::failf("no server called {} in grab.conf", name);
    return write_grab_conf(env, servers::set_value(env.grab_text, "grab", "default_remote", name));
}

std::expected<TestResult, std::string> test_connection(const Env& env, std::string_view name,
                                                       std::stop_token stop) {
    const auto* s = find_server(env.cfg, name);
    if (s == nullptr) return util::failf("no server called {} in grab.conf", name);
    auto remote = load_rclone_remote(env.rclone_conf, s->rclone_remote);
    if (!remote) return util::fail(remote.error());

    TestResult result;
    auto probe = proc::run_capture(servers::probe_argv(env.rclone, env.rclone_conf, s->rclone_remote), quiet(stop));
    if (stop.stop_requested()) return util::fail("cancelled");
    result.rclone_ok = ok(probe);
    if (!result.rclone_ok) {
        result.rclone_error = probe ? clean_rclone_error(probe->err) : probe.error();
        if (!env.secrets_error.empty()) result.rclone_error += " (" + env.secrets_error + ")";
    }

    if (resolve_find_method(*s, *remote) == FindMethod::ssh) {
        std::vector<std::string> options{"-o", "BatchMode=yes", "-o", "ConnectTimeout=15"};
        options.insert(options.end(), s->ssh_options.begin(), s->ssh_options.end());
        auto ssh = proc::run_capture(build_ssh_argv(env.cfg.ssh, *remote, options, "echo grab-ok"), quiet(stop));
        if (stop.stop_requested()) return util::fail("cancelled");
        result.ssh_ok = ok(ssh) && ssh->out.find("grab-ok") != std::string::npos;
        if (!*result.ssh_ok) result.ssh_error = ssh ? first_line(ssh->err) : ssh.error();
    }
    return result;
}

} // namespace grab::server_ops
