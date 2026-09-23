#include "server_cli.hpp"

#include "config.hpp"
#include "process.hpp"
#include "remote.hpp"
#include "servers.hpp"
#include "util.hpp"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <print>
#include <string>
#include <vector>

namespace grab {

namespace {

constexpr int exit_ok = 0;
constexpr int exit_usage = 1;
constexpr int exit_config = 2;
constexpr int exit_not_found = 3;

void error(std::string_view msg) { std::println(stderr, "grab: error: {}", msg); }

// Where everything lives, resolved once per command.
struct Env {
    std::filesystem::path grab_conf;
    std::filesystem::path rclone_conf;
    std::filesystem::path known_hosts;
    std::string rclone;
    GrabConfig cfg;
    std::string grab_text; // grab.conf as it is on disk (or a fresh [grab] block)
};

std::expected<Env, std::string> load_env(const Options& opts) {
    Env env;
    env.grab_conf = opts.config.value_or(default_grab_config_path());
    std::error_code ec;
    if (std::filesystem::exists(env.grab_conf, ec)) {
        auto text = util::read_file(env.grab_conf);
        if (!text) return util::fail(text.error());
        env.grab_text = std::move(*text);
        auto doc = ini::parse(env.grab_text);
        if (!doc) return util::failf("{}: {}", util::path_to_utf8(env.grab_conf), doc.error());
        auto cfg = parse_grab_config(*doc);
        if (!cfg) return util::failf("{}: {}", util::path_to_utf8(env.grab_conf), cfg.error());
        env.cfg = std::move(*cfg);
        auto imported = migrate_rclone_remotes(env.cfg);
        if (!imported.imported.empty()) {
            std::println(stderr, "grab: imported {} from rclone.conf into {}", util::join(imported.imported, ", "),
                         util::path_to_utf8(grab_rclone_config_path()));
        }
    } else {
        env.grab_text = generate_grab_config(nullptr, "");
    }
    env.rclone_conf = effective_rclone_config(env.cfg);
    env.known_hosts = grab_known_hosts_path();
    env.rclone = resolve_rclone_exe(env.cfg, util::self_exe_path().parent_path());
    return env;
}

bool write_text(const std::filesystem::path& p, const std::string& text) {
    std::error_code ec;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out || !(out << text)) return false;
    }
    std::filesystem::rename(tmp, p, ec);
    return !ec;
}

// Prompts on stderr; Enter keeps `def`. nullopt at end of input.
std::optional<std::string> ask(std::string_view prompt, std::string_view def = {}) {
    if (def.empty()) std::print(stderr, "{}: ", prompt);
    else std::print(stderr, "{} [{}]: ", prompt, def);
    std::fflush(stderr);
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    const auto t = util::trim(line);
    return t.empty() ? std::string(def) : std::string(t);
}

bool confirm(std::string_view prompt) {
    auto a = ask(std::format("{} [y/N]", prompt));
    if (!a) return false;
    const auto lower = util::to_lower(*a);
    return lower == "y" || lower == "yes";
}

std::string first_line(std::string_view text) {
    text = util::trim(text);
    const auto nl = text.find('\n');
    return std::string(util::trim(nl == std::string_view::npos ? text : text.substr(0, nl)));
}

proc::RunOptions quiet() {
    proc::RunOptions o;
    o.detached = true;
    return o;
}

// The server's host key lines via real ssh handshakes, one per key type (see
// servers::handshake_argv): the fallback for when ssh-keyscan cannot negotiate.
std::expected<std::vector<std::string>, std::string> handshake_host_key(const std::string& ssh,
                                                                        const std::string& host, int port) {
    std::error_code ec;
    std::string last_error;
    std::vector<std::string> lines;
    for (const auto& algorithm : servers::handshake_key_algorithms()) {
        // A fresh record file each time: once a host is known with one key type, accept-new
        // would not record another.
        const auto record = std::filesystem::temp_directory_path() /
                            std::format("grab-hostkey-{}-{}-{}.txt", host, port, algorithm);
        std::filesystem::remove(record, ec);
        auto run = proc::run_capture(servers::handshake_argv(ssh, host, port, record, algorithm), quiet());
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

// Scans the server's host keys, shows their fingerprints and asks to trust them. Returns the
// known_hosts lines to pin.

std::expected<std::vector<std::string>, std::string> fetch_host_key(const std::string& ssh,
                                                                    const std::string& host, int port) {
    std::println(stderr, "Fetching the host key of {}:{} ...", host, port);
    auto scan = proc::run_capture(servers::keyscan_argv(host, port), quiet());
    auto lines = scan ? servers::parse_keyscan(scan->out) : std::vector<std::string>{};
    if (lines.empty()) {
        auto shake = handshake_host_key(ssh, host, port);
        if (!shake) return util::fail(shake.error());
        lines = std::move(*shake);
    }
    auto opts = quiet();
    opts.input = util::join(lines, "\n") + "\n";
    auto fp = proc::run_capture(std::vector<std::string>{"ssh-keygen", "-lf", "-"}, opts);
    const auto prints = fp ? servers::parse_fingerprints(fp->out) : std::vector<servers::Fingerprint>{};

    std::println(stderr, "\nThe server presents these keys:");
    if (prints.empty()) {
        for (const auto& l : lines) std::println(stderr, "  {}", l);
    } else {
        for (const auto& f : prints) std::println(stderr, "  {:<8} {}", f.type, f.hash);
    }
    std::println(stderr, "Compare with the fingerprint your provider shows, or run on the server:");
    std::println(stderr, "  ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub\n");
    if (!confirm("Trust this server?")) return util::fail("host key not trusted; nothing was changed");
    return lines;
}

bool pin_host_keys(const std::filesystem::path& known_hosts, const std::vector<std::string>& lines) {
    std::string existing;
    if (auto t = util::read_file(known_hosts)) existing = std::move(*t);
    return write_text(known_hosts, servers::merge_known_hosts(existing, lines));
}

const RemoteSettings* find_server(const GrabConfig& cfg, std::string_view name) {
    for (const auto& r : cfg.remotes) {
        if (r.name == name) return &r;
    }
    return nullptr;
}

// ---- list ------------------------------------------------------------------------------------

int server_list(const Env& env) {
    if (env.cfg.remotes.empty()) {
        std::println("No servers yet. Add one with:  grab server add");
        return exit_ok;
    }
    std::println("Servers in {}  (connections: {})", util::path_to_utf8(env.grab_conf),
                 util::path_to_utf8(env.rclone_conf));
    for (const auto& s : env.cfg.remotes) {
        const bool is_default = env.cfg.default_remote == s.name ||
                                (!env.cfg.default_remote && env.cfg.remotes.size() == 1);
        const std::string mark = is_default ? "*" : " ";
        auto remote = load_rclone_remote(env.rclone_conf, s.rclone_remote);
        if (!remote) {
            std::println("{} {:<14} not usable: {}", mark, s.name, remote.error());
            continue;
        }
        const bool agent = remote->key_use_agent;
        const std::string auth = agent ? "ssh-agent" : remote->key_file ? "key file" : "password";
        const bool ssh = resolve_find_method(s, *remote) == FindMethod::ssh;
        const std::string hostkey = remote->known_hosts_file ? "host key pinned"
                                                             : "host key NOT checked (grab server trust " + s.name + ")";
        std::println("{} {:<14} {}@{}:{}  {}, search via {}, {}", mark, s.name, remote->user, remote->host,
                     remote->port, auth, ssh ? "ssh" : "rclone", hostkey);
    }
    return exit_ok;
}

// ---- add -------------------------------------------------------------------------------------

std::optional<int> parse_int(std::string_view s, int lo, int hi) {
    int v = 0;
    const auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
    if (ec != std::errc{} || ptr != s.data() + s.size() || v < lo || v > hi) return std::nullopt;
    return v;
}

// Re-asks until `valid` accepts the answer; nullopt at end of input.
template <class Valid>
std::optional<std::string> ask_until(std::string_view prompt, std::string_view def, Valid valid) {
    for (;;) {
        auto a = ask(prompt, def);
        if (!a) return std::nullopt;
        if (auto why = valid(*a)) {
            std::println(stderr, "  {}", *why);
            continue;
        }
        return a;
    }
}

// Answers come from the console, or from piped stdin one per line (scriptable); running out
// of input aborts without writing anything.
int server_add(const Env& env, const std::string& prefill) {
    auto rclone_doc = ini::parse(util::read_file(env.rclone_conf).value_or(""));
    auto taken = [&](const std::string& name) -> std::optional<std::string> {
        if (auto why = servers::validate_name(name)) return why;
        if (find_server(env.cfg, name) != nullptr) return "grab.conf already has a server called " + name;
        if (rclone_doc && rclone_doc->find(name) != nullptr) {
            return "grab's rclone.conf already has a remote called " + name;
        }
        return std::nullopt;
    };
    auto required = [](const std::string& v) -> std::optional<std::string> {
        return v.empty() ? std::optional<std::string>("required") : std::nullopt;
    };

    std::println(stderr, "Add a server (Enter keeps the value in brackets, Ctrl+C cancels).\n");
    auto aborted = [] {
        std::println(stderr, "\nno more input; nothing was added");
        return exit_usage;
    };
    servers::NewServer s;
    auto name = ask_until("Name (used with -r)", prefill, taken);
    if (!name) return aborted();
    s.name = *name;
    auto host = ask_until("Host name or IP", "", required);
    if (!host) return aborted();
    s.host = *host;
    auto port = ask_until("SSH port", "22", [](const std::string& v) -> std::optional<std::string> {
        return parse_int(v, 1, 65535) ? std::nullopt : std::optional<std::string>("a number from 1 to 65535");
    });
    if (!port) return aborted();
    s.port = *parse_int(*port, 1, 65535);
    auto user = ask_until("User name", "", required);
    if (!user) return aborted();
    s.user = *user;

    auto auth = ask_until("Log in with  1) password  2) key file  3) ssh-agent", "1",
                          [](const std::string& v) -> std::optional<std::string> {
                              return v == "1" || v == "2" || v == "3" ? std::nullopt
                                                                      : std::optional<std::string>("1, 2 or 3");
                          });
    if (!auth) return aborted();
    std::optional<std::string> secret;
    if (*auth == "1") {
        s.auth = servers::Auth::password;
        for (;;) {
            std::print(stderr, "Password (not shown): ");
            std::fflush(stderr);
            auto pw = util::read_secret_line();
            if (!pw) return aborted();
            if (!pw->empty()) {
                secret = std::move(*pw);
                break;
            }
            std::println(stderr, "  required");
        }
    } else if (*auth == "2") {
        s.auth = servers::Auth::key_file;
        auto key = ask_until("Private key file", "", [](const std::string& v) -> std::optional<std::string> {
            std::error_code ec;
            return std::filesystem::is_regular_file(util::path_from_utf8(v), ec)
                       ? std::nullopt
                       : std::optional<std::string>("no such file");
        });
        if (!key) return aborted();
        s.key_file = *key;
        std::print(stderr, "Key passphrase (not shown; Enter if none): ");
        std::fflush(stderr);
        auto pass = util::read_secret_line();
        if (!pass) return aborted();
        if (!pass->empty()) secret = std::move(*pass);
    } else {
        s.auth = servers::Auth::agent;
    }

    auto roots = ask("Folders to search, comma-separated (Enter = the login home)", "");
    if (!roots) return aborted();
    for (const auto& r : util::split(*roots, ',')) s.search_roots.push_back(normalize_root(r));
    auto depth = ask_until("Search depth", "4", [](const std::string& v) -> std::optional<std::string> {
        return parse_int(v, 1, 64) ? std::nullopt : std::optional<std::string>("a number from 1 to 64");
    });
    if (!depth) return aborted();
    s.max_depth = *parse_int(*depth, 1, 64);

    // Host key first: nothing is written unless the user trusts the server.
    auto keys = fetch_host_key(env.cfg.ssh, s.host, s.port);
    if (!keys) {
        error(keys.error());
        return exit_config;
    }

    std::optional<std::string> obscured;
    if (secret) {
        auto opts = quiet();
        opts.input = *secret; // via stdin: the plaintext never appears on a command line
        auto ob = proc::run_capture(servers::obscure_argv(env.rclone), opts);
        if (!ob || ob->exit_code != 0) {
            error(std::format("rclone obscure failed: {}", ob ? first_line(ob->err) : ob.error()));
            return exit_config;
        }
        obscured = first_line(ob->out);
    }

    if (!pin_host_keys(env.known_hosts, *keys)) {
        error(std::format("cannot write {}", util::path_to_utf8(env.known_hosts)));
        return exit_config;
    }
    std::error_code ec;
    std::filesystem::create_directories(env.rclone_conf.parent_path(), ec);
    auto created = proc::run_capture(servers::create_argv(env.rclone, env.rclone_conf, s, obscured, env.known_hosts),
                                     quiet());
    if (!created || created->exit_code != 0) {
        error(std::format("rclone config create failed: {}", created ? first_line(created->err) : created.error()));
        return exit_config;
    }

    const RcloneRemote remote = servers::to_remote(s, env.known_hosts);
    std::string text = servers::append_section(env.grab_text, remote_section(remote, s.search_roots, s.max_depth));
    if (!env.cfg.default_remote || env.cfg.remotes.empty()) {
        text = servers::set_value(text, "grab", "default_remote", s.name);
    }
    if (!write_text(env.grab_conf, text)) {
        error(std::format("cannot write {}", util::path_to_utf8(env.grab_conf)));
        return exit_config;
    }
    std::println(stderr, "\nSaved {} to {} and {}.", s.name, util::path_to_utf8(env.grab_conf),
                 util::path_to_utf8(env.rclone_conf));

    // Connection test: the same paths searches and downloads will use.
    std::println(stderr, "Testing the connection ...");
    auto probe = proc::run_capture(servers::probe_argv(env.rclone, env.rclone_conf, s.name), quiet());
    const bool rclone_ok = probe && probe->exit_code == 0;
    std::println(stderr, "  rclone (downloads): {}",
                 rclone_ok ? "OK" : "FAILED: " + (probe ? first_line(probe->err) : probe.error()));
    bool ssh_ok = true;
    if (s.auth != servers::Auth::password) {
        const std::vector<std::string> options{"-o", "BatchMode=yes", "-o", "ConnectTimeout=15"};
        auto ssh = proc::run_capture(build_ssh_argv(env.cfg.ssh, remote, options, "echo grab-ok"), quiet());
        ssh_ok = ssh && ssh->exit_code == 0 && ssh->out.find("grab-ok") != std::string::npos;
        std::string why = ssh ? first_line(ssh->err) : ssh.error();
        if (s.auth == servers::Auth::key_file && secret) why += " (a passphrase-protected key needs ssh-agent: ssh-add <key file>)";
        std::println(stderr, "  ssh (searches):     {}", ssh_ok ? "OK" : "FAILED: " + why);
    }
    if (rclone_ok && ssh_ok) {
        std::println(stderr, "\nReady:  grab -r {} WORDS [DEST]", s.name);
        return exit_ok;
    }
    std::println(stderr, "\nThe server is saved; fix the problem above, or remove it with: grab server remove {}", s.name);
    return exit_config;
}

// ---- trust / remove --------------------------------------------------------------------------

int server_trust(const Env& env, const std::string& name) {
    const auto* s = find_server(env.cfg, name);
    if (s == nullptr) {
        error(std::format("no server called {} in grab.conf", name));
        return exit_not_found;
    }
    auto remote = load_rclone_remote(env.rclone_conf, s->rclone_remote);
    if (!remote) {
        error(remote.error());
        return exit_config;
    }
    auto keys = fetch_host_key(env.cfg.ssh, remote->host, remote->port);
    if (!keys) {
        error(keys.error());
        return exit_config;
    }
    if (!pin_host_keys(env.known_hosts, *keys)) {
        error(std::format("cannot write {}", util::path_to_utf8(env.known_hosts)));
        return exit_config;
    }
    auto updated = proc::run_capture(
        servers::update_argv(env.rclone, env.rclone_conf, s->rclone_remote, "known_hosts_file",
                             util::path_to_utf8(env.known_hosts)),
        quiet());
    if (!updated || updated->exit_code != 0) {
        error(std::format("rclone config update failed: {}", updated ? first_line(updated->err) : updated.error()));
        return exit_config;
    }
    std::println(stderr, "Pinned the host key of {}; ssh and rclone now refuse a server that presents another.",
                 name);
    return exit_ok;
}

int server_remove(const Env& env, const std::string& name) {
    const auto* s = find_server(env.cfg, name);
    if (s == nullptr) {
        error(std::format("no server called {} in grab.conf", name));
        return exit_not_found;
    }
    // Confirmation comes from the console or piped stdin (`echo y | grab server remove NAME`);
    // no answer at all means no.
    if (!confirm(std::format("Remove {} from grab.conf and grab's rclone.conf?", name))) {
        std::println(stderr, "Nothing removed.");
        return exit_ok;
    }
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
    if (!write_text(env.grab_conf, text)) {
        error(std::format("cannot write {}", util::path_to_utf8(env.grab_conf)));
        return exit_config;
    }
    // Only grab's own rclone.conf is edited; an explicitly configured shared one is left alone.
    if (!env.cfg.rclone_config) {
        auto deleted = proc::run_capture(servers::delete_argv(env.rclone, env.rclone_conf, rclone_name), quiet());
        if (!deleted || deleted->exit_code != 0) {
            std::println(stderr, "note: rclone config delete {} failed: {}", rclone_name,
                         deleted ? first_line(deleted->err) : deleted.error());
        }
    }
    std::println(stderr, "Removed {}.", name);
    return exit_ok;
}

} // namespace

int run_server_command(const Options& opts) {
    auto env = load_env(opts);
    if (!env) {
        error(env.error());
        return exit_config;
    }
    const std::string& sub = opts.server_args.at(0);
    const std::string arg = opts.server_args.size() > 1 ? opts.server_args[1] : std::string{};
    if (sub == "list") return server_list(*env);
    if (sub == "add") return server_add(*env, arg);
    if (sub == "trust") return server_trust(*env, arg);
    if (sub == "remove") return server_remove(*env, arg);
    error(std::format("unknown `grab server` command '{}'", sub));
    return exit_usage;
}

} // namespace grab
