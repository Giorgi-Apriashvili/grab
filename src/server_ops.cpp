#include "server_ops.hpp"

#include "conn_budget.hpp"
#include "engine.hpp"
#include "gui_state.hpp"
#include "process.hpp"
#include "remote.hpp"
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

// Whether a remote in the rclone.conf in use (as it is now) still needs host:port's keys in grab's
// known_hosts: the remote of a grab server but `except` (two logins on one machine share its host
// keys), or any remote whose known_hosts_file is grab's, such as one an explicit rclone_config
// keeps after its server left grab. Host names as written, as known_hosts readers compare them.
// An rclone.conf that can't be read counts as needing them.
bool used_by_another(const Env& env, std::string_view host, int port, std::string_view except) {
    auto text = util::read_file_if_exists(env.rclone_conf);
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
        const bool grab_server = std::ranges::any_of(env.cfg.remotes,
                                                     [&](const auto& r) { return r.name != except && r.rclone_remote == section.name; });
        const auto file = section.get("known_hosts_file");
        const bool uses_grab_file =
            file && !util::trim(*file).empty() &&
            util::to_lower(util::path_to_utf8(util::path_from_utf8(util::trim(*file)).lexically_normal())) == grab_known;
        if (grab_server || uses_grab_file) return true;
    }
    return false;
}

// host:port's keys out of grab's known_hosts once no remote needs them but the grab server
// `except`'s (review G87): why not, else "".
std::string forget_unused_keys(const Env& env, std::string_view host, int port, std::string_view except) {
    if (host.empty() || used_by_another(env, host, port, except)) return {};
    auto forgot = forget_host_keys(env.known_hosts, host, port);
    return forgot ? std::string{} : std::format("its host keys could not be removed from {}: {}",
                                                util::path_to_utf8(env.known_hosts), forgot.error());
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

// Changes the encrypted store of grab's own rclone.conf, which each rclone run reads its server's
// secrets from (engine::rclone_env). A store that can't be decrypted starts over, the old file set
// aside (secrets::load_for_change); the note returned then says so, and which other servers (but
// `leaving`, one being removed) need their password again (review G211). Empty otherwise.
std::expected<std::string, std::string> change_secrets(const Env& env, const std::function<void(secrets::Store&)>& change,
                                                       std::string_view leaving = {}) {
    auto lock = lock_settings();
    if (!lock) return util::fail(lock.error());
    const auto file = secrets::path_beside(env.rclone_conf);
    auto loaded = secrets::load_for_change(file);
    if (!loaded) return util::fail(loaded.error());
    change(loaded->store);
    if (auto saved = secrets::save(file, loaded->store); !saved) return util::fail(saved.error());
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
            info.engine = effective_engine(s);
            info.search = std::string(search_method(s, *remote, info.engine));
            info.host_key_pinned = remote->known_hosts_file.has_value();
            info.default_connections = budget::default_budget(remote->host);
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

    if (auto pinned = pin_host_keys(env.known_hosts, s.host, s.port, keys.lines); !pinned) {
        return util::fail(pinned.error());
    }
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
    // The address it had, whose keys go once it has moved and no other server is there.
    const auto before = load_rclone_remote(env.rclone_conf, settings->rclone_remote);
    if (keys) {
        if (auto pinned = pin_host_keys(env.known_hosts, wanted.host, wanted.port, keys->lines); !pinned) {
            return util::fail(pinned.error());
        }
        const std::string known = util::path_to_utf8(env.known_hosts);
        if (section->get("known_hosts_file").value_or("") != known) updates.emplace_back("known_hosts_file", known);
    }
    if (!updates.empty()) {
        auto run = proc::run_capture(servers::update_argv(env.rclone, env.rclone_conf, settings->rclone_remote, updates),
                                     quiet());
        if (!ok(run)) return run_failed("rclone config update", run);
    }
    std::vector<std::string> notes;
    if (before && (before->host != wanted.host || before->port != wanted.port)) {
        if (auto why = forget_unused_keys(env, before->host, before->port, wanted.name); !why.empty()) {
            notes.push_back(std::move(why));
        }
    }
    if (own) {
        auto kept = change_secrets(env, [&](secrets::Store& st) {
            apply_auth(st, settings->rclone_remote, wanted.auth, obscured_secret);
        });
        if (!kept) return util::fail(kept.error());
        if (!kept->empty()) notes.push_back(std::move(*kept));
    }
    const std::string note = util::join(notes, "; ");

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
    text = servers::replace_lead_in(
        text, wanted.name, std::format("# rclone remote [{}]: {}", remote.name, describe_remote(remote, *settings)));
    if (auto written = write_grab_conf(env, text); !written) return util::fail(written.error());
    return note;
}

std::expected<void, std::string> trust_server(const Env& loaded, std::string_view name, const HostKeys& keys) {
    auto locked = lock_current(loaded);
    if (!locked) return util::fail(locked.error());
    const Env& env = locked->env;
    const auto* s = find_server(env.cfg, name);
    if (s == nullptr) return util::failf("no server called {} in grab.conf", name);
    auto remote = load_rclone_remote(env.rclone_conf, s->rclone_remote);
    if (!remote) return util::fail(remote.error());
    if (auto pinned = pin_host_keys(env.known_hosts, remote->host, remote->port, keys.lines); !pinned) return pinned;
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
    const auto remote = load_rclone_remote(env.rclone_conf, rclone_name); // its address, for its keys
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
    // The server is gone from grab either way; what couldn't be cleaned up is said.
    std::vector<std::string> notes;
    // Then its host keys leave grab's own known_hosts unless a remote still needs them (review G87):
    // one left in an explicit rclone_config keeps them when it points at grab's known_hosts.
    auto forget_keys = [&] {
        if (!remote) return;
        if (auto why = forget_unused_keys(env, remote->host, remote->port, name); !why.empty()) {
            notes.push_back(std::move(why));
        }
    };
    // Only grab's own rclone.conf is edited; an explicitly configured shared one is left alone.
    if (env.cfg.rclone_config) {
        forget_keys();
        return util::join(notes, "; ");
    }
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
    forget_keys();
    return util::join(notes, "; ");
}

std::expected<void, std::string> set_default(const Env& loaded, std::string_view name) {
    auto locked = lock_current(loaded);
    if (!locked) return util::fail(locked.error());
    const Env& env = locked->env;
    if (find_server(env.cfg, name) == nullptr) return util::failf("no server called {} in grab.conf", name);
    return write_grab_conf(env, servers::set_value(env.grab_text, "grab", "default_remote", name));
}

namespace {

// grab's own client's part of a test: a login through it (the target downloads use), then on
// engine = grab the home folder listed and the find check on that connection, which says how
// searches go; its answer replaces the one saved in `limits_file` (engine::find_memory), so a
// server changed since is searched the new way by the next command (the window drops its pool's
// answer, App::test_server). On every server the hash check last, which says whether downloads can
// be verified there (phase 5). Nothing is set when stopped.
void test_grab(const engine::Context& ctx, const std::filesystem::path& limits_file, const std::stop_token& stop,
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
        if (grab_engine) {
            if (auto listed = sftp::list((*lease)->sftp(), "."); !listed) {
                return failed("its home folder can't be listed: " + sftp::describe(listed.error()));
            }
        }
    } catch (const ssh::Error& e) {
        return failed(e.what());
    } catch (const sftp::Broken& e) {
        return failed(e.what());
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
    // Whether verification can run: rclone.conf's keys, then the server's md5sum.
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
    auto remote = load_rclone_remote(env.rclone_conf, s->rclone_remote);
    if (!remote) return util::fail(remote.error());

    TestResult result;
    const Engine server_engine = effective_engine(*s);
    engine::Context ctx{.config = env.cfg,
                        .settings = *s,
                        .remote = *remote,
                        .method = resolve_find_method(*s, *remote),
                        .imported = {},
                        .engine = server_engine,
                        .grab_method = resolve_grab_method(*s, *remote)};
    ctx.config.rclone = env.rclone;
    ctx.config.rclone_config = env.rclone_conf; // as load_context resolves them
    // rclone only where it still downloads: on engine = grab nothing a test can show goes through it
    // (previews and Play don't either since phase 6), and its listing costs a login (1-3.5 s on a
    // Storage Box).
    if (server_engine != Engine::grab) {
        auto probe = engine::rclone_env(ctx).and_then([&](proc::Environment server) {
            auto run = quiet(stop);
            run.environment = std::move(server);
            return proc::run_capture(servers::probe_argv(env.rclone, s->rclone_remote), run);
        });
        if (stop.stop_requested()) return util::fail("cancelled");
        result.rclone_ok = ok(probe);
        if (!*result.rclone_ok) {
            result.rclone_error = probe ? clean_rclone_error(probe->err) : probe.error();
            if (!env.secrets_error.empty()) result.rclone_error += " (" + env.secrets_error + ")";
        }
    }

    if (server_engine != Engine::grab && resolve_find_method(*s, *remote) == FindMethod::ssh) {
        std::vector<std::string> options{"-o", "BatchMode=yes", "-o", "ConnectTimeout=15"};
        options.insert(options.end(), s->ssh_options.begin(), s->ssh_options.end());
        auto ssh = proc::run_capture(build_ssh_argv(env.cfg.ssh, *remote, options, "echo grab-ok"), quiet(stop));
        if (stop.stop_requested()) return util::fail("cancelled");
        result.ssh_ok = ok(ssh) && ssh->out.find("grab-ok") != std::string::npos;
        if (!*result.ssh_ok) result.ssh_error = ssh ? first_line(ssh->err) : ssh.error();
    }
    // Every server: grab's own login, which downloads and searches use on engine = grab, and
    // previews, Play and verification on any.
    test_grab(ctx, learned_limits_path(env.grab_conf), stop, result);
    if (stop.stop_requested()) return util::fail("cancelled");
    return result;
}

} // namespace grab::server_ops
