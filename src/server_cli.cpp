#include "server_cli.hpp"

#include "config.hpp"
#include "server_ops.hpp"
#include "servers.hpp"
#include "util.hpp"

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <format>
#include <iostream>
#include <print>
#include <string>
#include <vector>

namespace grab {

namespace {

using server_ops::Env;

constexpr int exit_ok = 0;
constexpr int exit_usage = 1;
constexpr int exit_config = 2;
constexpr int exit_not_found = 3;

void error(std::string_view msg) { std::println(stderr, "grab: error: {}", msg); }

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

// Scans the server's host keys, shows their fingerprints and asks to trust them.
std::expected<server_ops::HostKeys, std::string> fetch_host_key(const std::string& ssh, const std::string& host,
                                                                int port) {
    std::println(stderr, "Fetching the host key of {}:{} ...", host, port);
    auto keys = server_ops::scan_host_keys(ssh, host, port);
    if (!keys) return keys;

    std::println(stderr, "\nThe server presents these keys:");
    if (keys->fingerprints.empty()) {
        for (const auto& l : keys->lines) std::println(stderr, "  {}", l);
    } else {
        for (const auto& f : keys->fingerprints) std::println(stderr, "  {:<8} {}", f.type, f.hash);
    }
    std::println(stderr, "Compare with the fingerprint your provider shows, or run on the server:");
    std::println(stderr, "  ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub\n");
    if (!confirm("Trust this server?")) return util::fail("host key not trusted; nothing was changed");
    return keys;
}

// ---- list ------------------------------------------------------------------------------------

int server_list(const Env& env) {
    if (env.cfg.remotes.empty()) {
        std::println("No servers yet. Add one with:  grab server add");
        return exit_ok;
    }
    std::println("Servers in {}  (connections: {})", util::path_to_utf8(env.grab_conf),
                 util::path_to_utf8(env.rclone_conf));
    for (const auto& s : server_ops::list_servers(env)) {
        const std::string mark = s.is_default ? "*" : " ";
        if (!s.error.empty()) {
            std::println("{} {:<14} not usable: {}", mark, s.name, s.error);
            continue;
        }
        const char* auth = s.auth == servers::Auth::agent      ? "ssh-agent"
                           : s.auth == servers::Auth::key_file ? "key file"
                                                               : "password";
        const std::string hostkey =
            s.host_key_pinned ? "host key pinned" : "host key NOT checked (grab server trust " + s.name + ")";
        std::println("{} {:<14} {}@{}:{}  {}, search via {}, {}", mark, s.name, s.user, s.host, s.port, auth,
                     s.ssh_search ? "ssh" : "rclone", hostkey);
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
        if (server_ops::find_server(env.cfg, name) != nullptr) return "grab.conf already has a server called " + name;
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
        auto ob = server_ops::obscure(env, *secret);
        if (!ob) {
            error(ob.error());
            return exit_config;
        }
        obscured = std::move(*ob);
    }
    if (auto added = server_ops::add_server(env, s, obscured, *keys); !added) {
        error(added.error());
        return exit_config;
    }
    std::println(stderr, "\nSaved {} to {} and {}.", s.name, util::path_to_utf8(env.grab_conf),
                 util::path_to_utf8(env.rclone_conf));

    // Connection test: the same paths searches and downloads will use. The env is reloaded so
    // it sees the server just written.
    std::println(stderr, "Testing the connection ...");
    auto fresh = server_ops::load_env(env.grab_conf, util::self_exe_path().parent_path());
    if (!fresh) {
        error(fresh.error());
        return exit_config;
    }
    auto test = server_ops::test_connection(*fresh, s.name);
    if (!test) {
        error(test.error());
        return exit_config;
    }
    std::println(stderr, "  rclone (downloads): {}", test->rclone_ok ? "OK" : "FAILED: " + test->rclone_error);
    if (test->ssh_ok) {
        std::string why = test->ssh_error;
        if (s.auth == servers::Auth::key_file && secret) {
            why += " (a passphrase-protected key needs ssh-agent: ssh-add <key file>)";
        }
        std::println(stderr, "  ssh (searches):     {}", *test->ssh_ok ? "OK" : "FAILED: " + why);
    }
    if (test->ok()) {
        std::println(stderr, "\nReady:  grab -r {} WORDS [DEST]", s.name);
        return exit_ok;
    }
    std::println(stderr, "\nThe server is saved; fix the problem above, or remove it with: grab server remove {}", s.name);
    return exit_config;
}

// ---- trust / remove --------------------------------------------------------------------------

int server_trust(const Env& env, const std::string& name) {
    const auto* s = server_ops::find_server(env.cfg, name);
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
    if (auto trusted = server_ops::trust_server(env, name, *keys); !trusted) {
        error(trusted.error());
        return exit_config;
    }
    std::println(stderr, "Pinned the host key of {}; ssh and rclone now refuse a server that presents another.",
                 name);
    return exit_ok;
}

int server_remove(const Env& env, const std::string& name) {
    if (server_ops::find_server(env.cfg, name) == nullptr) {
        error(std::format("no server called {} in grab.conf", name));
        return exit_not_found;
    }
    // Confirmation comes from the console or piped stdin (`echo y | grab server remove NAME`);
    // no answer at all means no.
    if (!confirm(std::format("Remove {} from grab.conf and grab's rclone.conf?", name))) {
        std::println(stderr, "Nothing removed.");
        return exit_ok;
    }
    auto removed = server_ops::remove_server(env, name);
    if (!removed) {
        error(removed.error());
        return exit_config;
    }
    if (!removed->empty()) std::println(stderr, "note: {}", *removed);
    std::println(stderr, "Removed {}.", name);
    return exit_ok;
}

} // namespace

int run_server_command(const Options& opts) {
    auto env = server_ops::load_env(opts.config.value_or(default_grab_config_path()),
                                    util::self_exe_path().parent_path());
    if (!env) {
        error(env.error());
        return exit_config;
    }
    if (!env->imported.empty()) {
        std::println(stderr, "grab: imported {} from rclone.conf into {}", util::join(env->imported, ", "),
                     util::path_to_utf8(grab_rclone_config_path()));
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
