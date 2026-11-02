#include "server_cli.hpp"

#include "config.hpp"
#include "guard.hpp"
#include "gui_state.hpp"
#include "notices.hpp"
#include "server_conf.hpp"
#include "server_ops.hpp"
#include "servers.hpp"
#include "util.hpp"

#include <algorithm>
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

// Scans the server's host keys, shows their fingerprints and asks to trust them. Nothing is
// written before the yes. `reach`: the server's bind and connect time; `direct`, when its settings
// route its connections (server_ops::direct_warning), is said first and the scan waits for a yes,
// since it connects directly.
std::expected<server_ops::HostKeys, std::string> fetch_host_key(const std::string& host, int port,
                                                                const ssh::Reach& reach = {},
                                                                const std::string& direct = {}) {
    if (!direct.empty()) {
        std::println(stderr, "{}.", direct);
        if (!confirm("Connect directly?")) return util::fail("not connected; nothing was changed");
    }
    std::println(stderr, "Fetching the host key of {}:{} ...", host, port);
    auto keys = server_ops::scan_host_keys(host, port, {}, reach);
    if (!keys) return keys;

    std::println(stderr, "\nThe server presents these keys:");
    for (const auto& f : keys->fingerprints) std::println(stderr, "  {:<8} {}", f.type, f.hash);
    std::println(stderr, "Compare with the fingerprint your provider shows, or run on the server:");
    std::println(stderr, "  ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub\n");
    if (!confirm("Trust this server?")) return util::fail("host key not trusted; nothing was changed");
    return keys;
}

// The server called `name` as `grab server list` shows it, or nullopt with the error printed.
std::optional<server_ops::ServerInfo> server_named(const Env& env, std::string_view name) {
    auto list = server_ops::list_servers(env);
    const auto s = std::ranges::find(list, name, &server_ops::ServerInfo::name);
    if (s == list.end()) {
        error(std::format("no server called {} in grab.conf", name));
        return std::nullopt;
    }
    return std::move(*s);
}

// ---- list ------------------------------------------------------------------------------------

int server_list(const Env& env) {
    if (env.cfg.remotes.empty()) {
        std::println("No servers yet. Add one with:  grab server add");
        return exit_ok;
    }
    std::println("Servers in {}", util::path_to_utf8(env.grab_conf));
    for (const auto& s : server_ops::list_servers(env)) {
        const std::string mark = s.is_default ? "*" : " ";
        if (!s.error.empty()) {
            std::println("{} {:<14} not usable: {}", mark, s.name, s.error);
            continue;
        }
        // Each server's engine: on grab's own client (the default) it searches with find or by
        // listing folders; on rclone, the fallback (engine = rclone, or settings grab's client can't
        // serve, said below), ssh.exe runs find or rclone lists.
        const char* search = s.search == "ssh"    ? "engine rclone (fallback), search with ssh + find"
                             : s.search == "rclone" ? "engine rclone (fallback), search by rclone's listing"
                             : s.search == "find"   ? "engine grab, search with find"
                             : s.search == "walk"   ? "engine grab, search by listing folders"
                                                    : "engine grab, search with find or by listing folders";
        std::println("{} {:<14} {}@{}:{}  {}, {}, {}{}", mark, s.name, s.user, s.host, s.port, server_ops::login_text(s),
                     search, server_ops::host_key_text(s),
                     s.connection_keys.empty() ? std::string() : ", " + s.connection_keys);
        if (!s.guard.empty()) std::println("  {:<14} {}", "", guard::text(s.name, s.guard, s.engine));
    }
    return exit_ok;
}

// ---- add / edit / test -----------------------------------------------------------------------

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

// A secret typed without echo; Enter is an empty answer. nullopt at end of input.
std::optional<std::string> ask_secret(std::string_view prompt) {
    std::print(stderr, "{}: ", prompt);
    std::fflush(stderr);
    return util::read_secret_line();
}

// The questions `grab server add` and `edit` share, from the host to the search depth, with `s`
// as the defaults; the answers go into `s`, a password or passphrase typed into `secret`.
// `current` is the server being edited (nullptr for a new one): Enter keeps its saved password
// only while user, host and port stay the same (server_ops::edit_needs_password), and its saved
// passphrase while it logs in with a key file. False when the input ran out.
bool ask_details(servers::NewServer& s, std::optional<std::string>& secret, const server_ops::ServerInfo* current) {
    using servers::Auth;
    auto host = ask_until("Host name or IP", s.host, [](const std::string& v) { return server_conf::check_host(v); });
    if (!host) return false;
    s.host = *host;
    auto port = ask_until("SSH port", std::to_string(s.port), [](const std::string& v) -> std::optional<std::string> {
        return parse_int(v, 1, 65535) ? std::nullopt : std::optional<std::string>("a number from 1 to 65535");
    });
    if (!port) return false;
    s.port = *parse_int(*port, 1, 65535);
    auto user = ask_until("User name", s.user, [](const std::string& v) { return server_conf::check_user(v); });
    if (!user) return false;
    s.user = *user;

    // An imported key kept in grab's store can only be kept, never chosen.
    const bool stored = current != nullptr && s.auth == Auth::stored_key;
    const std::string choices = stored ? "1) password  2) key file  3) ssh-agent  4) the key kept in grab's store"
                                       : "1) password  2) key file  3) ssh-agent";
    const std::string_view was = s.auth == Auth::key          ? "2"
                                 : s.auth == Auth::agent      ? "3"
                                 : s.auth == Auth::stored_key ? "4"
                                                              : "1";
    auto auth = ask_until("Log in with  " + choices, was, [&](const std::string& v) -> std::optional<std::string> {
        if (v == "1" || v == "2" || v == "3" || (stored && v == "4")) return std::nullopt;
        return stored ? "1, 2, 3 or 4" : "1, 2 or 3";
    });
    if (!auth) return false;
    const Auth before = s.auth;
    const std::string before_key = s.key_file;
    s.auth = *auth == "1" ? Auth::password : *auth == "2" ? Auth::key : *auth == "3" ? Auth::agent : Auth::stored_key;
    // Beside ssh-agent an edit keeps the key file the section names (which agent key to use).
    s.key_file.clear();
    if (s.auth == Auth::password) {
        const bool keeps = current != nullptr && !server_ops::edit_needs_password(*current, s);
        for (;;) {
            auto pw = ask_secret(keeps ? "Password (not shown; Enter keeps the saved one)" : "Password (not shown)");
            if (!pw) return false;
            if (!pw->empty()) {
                secret = std::move(*pw);
                break;
            }
            if (keeps) break;
            std::println(stderr, "  required");
        }
    } else if (s.auth == Auth::key) {
        auto key = ask_until("Private key file", before == Auth::key || before == Auth::agent ? before_key : "",
                             [](const std::string& v) {
                                 std::error_code ec;
                                 return server_conf::check_key_file(
                                     v, std::filesystem::is_regular_file(util::path_from_utf8(v), ec));
                             });
        if (!key) return false;
        s.key_file = *key;
        // Another key file drops the old one's passphrase (server_ops::update_server).
        const bool keeps = current != nullptr && before == Auth::key && *key == before_key &&
                           current->secret_saved.value_or(false);
        auto pass = ask_secret(keeps ? "Key passphrase (not shown; Enter keeps the saved one)"
                                     : "Key passphrase (not shown; Enter if none)");
        if (!pass) return false;
        if (!pass->empty()) secret = std::move(*pass);
    }

    auto roots = ask_until(current != nullptr ? "Folders to search, comma-separated (~ = the login home)"
                                              : "Folders to search, comma-separated (Enter = the login home)",
                           search_roots_value(s.search_roots),
                           [](const std::string& v) { return server_conf::check_value(v); });
    if (!roots) return false;
    s.search_roots.clear();
    for (const auto& r : util::split(*roots, ',')) s.search_roots.push_back(normalize_root(r));
    auto depth = ask_until("Search depth", std::to_string(s.max_depth), [](const std::string& v) -> std::optional<std::string> {
        return parse_int(v, 1, 64) ? std::nullopt : std::optional<std::string>("a number from 1 to 64");
    });
    if (!depth) return false;
    s.max_depth = *parse_int(*depth, 1, 64);
    return true;
}

// The connection test, printed: the same paths searches, downloads, previews, Play and
// verification use. A failed ssh login gets `ssh_hint`. exit_ok when it passed.
int print_test(const Env& env, const std::string& name, std::string_view ssh_hint) {
    auto test = server_ops::test_connection(env, name);
    if (!test) {
        error(test.error());
        return exit_config;
    }
    for (const auto& line : server_ops::test_lines(*test, ssh_hint)) std::println(stderr, "  {}", line);
    return test->ok() ? exit_ok : exit_config;
}

// After a change: the test, with the configuration read again so that it sees what was written.
int test_saved(const Env& env, const std::string& name, std::string_view ssh_hint) {
    std::println(stderr, "Testing the connection ...");
    auto fresh = server_ops::load_env(env.grab_conf, util::self_exe_path().parent_path());
    if (!fresh) {
        error(fresh.error());
        return exit_config;
    }
    if (print_test(*fresh, name, ssh_hint) == exit_ok) {
        std::println(stderr, "\nReady:  grab -r {} WORDS [DEST]", name);
        return exit_ok;
    }
    std::println(stderr,
                 "\nThe server is saved; fix the problem above with `grab server edit {}`, or remove it with: grab "
                 "server remove {}",
                 name, name);
    return exit_config;
}

// What a passphrase-protected key means for ssh + find, after its failed login.
std::string_view ssh_hint(servers::Auth auth, bool passphrase) {
    return auth == servers::Auth::key && passphrase ? " (a passphrase-protected key needs ssh-agent: ssh-add <key file>)"
                                                    : "";
}

// Answers come from the console, or from piped stdin one per line (scriptable); running out
// of input aborts without writing anything.
int server_add(const Env& env, const std::string& prefill) {
    auto taken = [&](const std::string& name) { return server_ops::name_taken(env, name); };

    std::println(stderr, "Add a server (Enter keeps the value in brackets, Ctrl+C cancels).\n");
    auto aborted = [] {
        std::println(stderr, "\nno more input; nothing was added");
        return exit_usage;
    };
    servers::NewServer s;
    auto name = ask_until("Name (used with -r)", prefill, taken);
    if (!name) return aborted();
    s.name = *name;
    std::optional<std::string> secret;
    if (!ask_details(s, secret, nullptr)) return aborted();

    // Host key first: nothing is written unless the user trusts the server.
    auto keys = fetch_host_key(s.host, s.port);
    if (!keys) {
        error(keys.error());
        return exit_config;
    }

    auto added = server_ops::add_server(env, s, secret, *keys);
    if (!added) {
        error(added.error());
        return exit_config;
    }
    if (!added->empty()) std::println(stderr, "note: {}", *added);
    std::println(stderr, "\nSaved {} to {}.", s.name, util::path_to_utf8(env.grab_conf));
    return test_saved(env, s.name, ssh_hint(s.auth, secret.has_value()));
}

// The add questions with the server's answers as defaults; its name stays (it keys partial
// downloads, lists, limits and the store). A section without connection details is filled in
// this way. Nothing is written when the input runs out or the host key isn't trusted.
int server_edit(const Env& env, const std::string& name) {
    const auto current = server_named(env, name);
    if (!current) return exit_not_found;
    std::println(stderr, "Edit {} (Enter keeps the value in brackets, Ctrl+C cancels).\n", name);
    if (!current->error.empty()) std::println(stderr, "{}\n", current->error);
    servers::NewServer s = server_ops::edit_defaults(*current);
    std::optional<std::string> secret;
    if (!ask_details(s, secret, &*current)) {
        std::println(stderr, "\nno more input; nothing was changed");
        return exit_usage;
    }

    // A new address, or one never pinned: its host key first, as for add.
    std::optional<server_ops::HostKeys> keys;
    if (server_ops::edit_needs_host_key(*current, s)) {
        auto reach = server_ops::reach_for(env, name);
        if (!reach) {
            error(reach.error());
            return exit_config;
        }
        auto fetched = fetch_host_key(s.host, s.port, *reach, server_ops::direct_warning(*current, s.host, s.port));
        if (!fetched) {
            error(fetched.error());
            return exit_config;
        }
        keys = std::move(*fetched);
    }
    auto done = server_ops::update_server(env, s, secret, keys);
    if (!done) {
        error(done.error());
        return exit_config;
    }
    if (!done->empty()) std::println(stderr, "note: {}", *done);
    std::println(stderr, "\nSaved {} in {}.", name, util::path_to_utf8(env.grab_conf));
    const bool passphrase = secret.has_value() || (current->auth == servers::Auth::key && s.key_file == current->key_file &&
                                                   current->secret_saved == true);
    return test_saved(env, name, ssh_hint(s.auth, passphrase));
}

// `again`: the server's notices (notices.hpp) not printed already, which a test repeats.
int server_test(const Env& env, const std::string& name, const std::vector<std::string>& again) {
    const auto s = server_named(env, name);
    if (!s) return exit_not_found;
    if (!s->error.empty()) {
        error(s->error);
        return exit_config;
    }
    std::println(stderr, "Testing {} ({}@{}:{}) ...", name, s->user, s->host, s->port);
    const int rc = print_test(env, name, ssh_hint(s->auth, s->secret_saved == true));
    for (const auto& text : again) std::println(stderr, "  note: {}", text);
    if (rc == exit_ok) std::println(stderr, "\n{} works.", name);
    else std::println(stderr, "\n{} has a problem: see above (grab server edit {} changes it).", name, name);
    return rc;
}

// ---- trust / remove --------------------------------------------------------------------------

int server_trust(const Env& env, const std::string& name) {
    const auto s = server_named(env, name);
    if (!s) return exit_not_found;
    if (!s->error.empty()) {
        error(s->error);
        return exit_config;
    }
    auto reach = server_ops::reach_for(env, name);
    if (!reach) {
        error(reach.error());
        return exit_config;
    }
    auto keys = fetch_host_key(s->host, s->port, *reach, server_ops::direct_warning(*s, s->host, s->port));
    if (!keys) {
        error(keys.error());
        return exit_config;
    }
    if (auto trusted = server_ops::trust_server(env, name, *keys); !trusted) {
        error(trusted.error());
        return exit_config;
    }
    std::println(stderr,
                 "Pinned the host key of {} in place of any it had; grab, ssh and rclone now refuse a server that "
                 "presents another.",
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
    if (!confirm(std::format("Remove {} from grab.conf, with its saved password, host keys and file list?", name))) {
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
        std::println(stderr, "grab: moved the connection details of {} from rclone.conf into {} (one time; rclone.conf "
                             "unchanged)",
                     util::join(env->imported, ", "), util::path_to_utf8(env->grab_conf));
    }
    for (const auto& note : env->import_notes) std::println(stderr, "grab: note: {}", note);
    const std::string& sub = opts.server_args.at(0);
    const std::string arg = opts.server_args.size() > 1 ? opts.server_args[1] : std::string{};
    // The settings' notices this configuration hasn't shown yet; a test repeats its server's.
    std::vector<std::string> again;
    if (auto all = notices::for_config(env->grab_conf)) {
        auto fresh = notices::take_new(notices::path_in(state_dir(env->grab_conf)), *all);
        const auto& shown = fresh ? *fresh : *all; // unsaved: said every time rather than never
        for (const auto& n : shown) std::println(stderr, "grab: note: {}", n.text);
        for (const auto& n : *all) {
            if (n.server == arg && !std::ranges::contains(shown, n)) again.push_back(n.text);
        }
    }
    if (sub == "list") return server_list(*env);
    if (sub == "add") return server_add(*env, arg);
    if (sub == "edit") return server_edit(*env, arg);
    if (sub == "test") return server_test(*env, arg, again);
    if (sub == "trust") return server_trust(*env, arg);
    if (sub == "remove") return server_remove(*env, arg);
    error(std::format("unknown `grab server` command '{}'", sub));
    return exit_usage;
}

} // namespace grab
