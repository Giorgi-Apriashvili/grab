#pragma once

// The servers grab can't serve as their settings say (RCLONE-REPLACEMENT.md, phase 8 step 4 and
// phase 9 step 6): an ssh command, or a proxy where grab doesn't read one (grab would connect
// directly, past the route set up for it), another SFTP subsystem or server command (it would see
// another server's files), a certificate login, insecure ciphers or algorithm lists that share
// nothing with grab's offer, and the switches for servers that need one read at a time or fstat.
// With rclone gone there is nothing to send such a server to: grab refuses it everywhere
// (downloads, searches, lists, Browse, previews, Play, Verify, Test, Trust and the host-key fetch
// of Add and Edit) with the reason, which names the setting and how to clear it. ssh_options'
// ProxyCommand, ProxyJump and -J only ever routed ssh.exe's searches, never rclone's downloads:
// they are read, but never a reason (the user's decision 4: a one-time note, and grab connects
// directly). Values decide, never a key's presence ("subsystem = sftp", "use_fstat = false" and
// "ProxyCommand none" are fine), and no reason carries a value: a proxy can hold a user:password.
//
// Proxies (PROXIES.md 3.3): a server's proxy is its grab.conf `proxy` line, else the SOCKS5 or
// HTTP proxy grab's store keeps for its login in rclone's form (the import's, which the user's
// decision 4 uses again, or grab's copy for older grabs, decision 6); route() says which. A kept
// one saved for another login, two kept ones, or one that isn't a proxy grab can use refuse the
// server, as do rclone's proxies in common_flags, folder_flags and the inherited RCLONE_SFTP_*
// variables (grab doesn't read them there). Pure functions.

#include "config.hpp"
#include "passwords.hpp"
#include "proxy.hpp"
#include "server_conf.hpp"

#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace grab::guard {

enum class Kind {
    route,       // a proxy, an ssh command; a ProxyCommand or ProxyJump (never a reason)
    server,      // another SFTP subsystem or server command
    certificate, // pubkey, pubkey_file
    cipher,      // use_insecure_cipher; ciphers, key exchanges, MACs or host key types grab doesn't offer
    quirk,       // disable_concurrent_reads, use_fstat
};

// Where a setting is, which says how to clear it.
enum class Origin {
    store,        // grab's store, where the import from rclone.conf kept it (rclone.<key>)
    line,         // a rclone.<key> line of the server's grab.conf section
    environment,  // an inherited RCLONE_SFTP_<KEY>, which covers every server
    common_flags, // common_flags' --sftp-<key>
    folder_flags, // folder_flags' --sftp-<key> (grab 0.19's folder downloads ran rclone with them)
    ssh_options,  // ssh_options' -o ProxyCommand, -o ProxyJump, -J: read, never a reason
    unreadable,   // grab's store can't be read: what it keeps is unknown
};

// One setting that keeps grab off a server.
struct Reason {
    Kind kind = Kind::route;
    std::string key; // rclone's option ("socks_proxy"); "store" for a store that can't be read
    // Where it is set, as texts name it: "rclone.socks_proxy" (a grab.conf line),
    // "rclone.socks_proxy in grab's store", "RCLONE_SFTP_SOCKS_PROXY", "common_flags'
    // --sftp-socks-proxy", "grab's store, which can't be read".
    std::string where;
    Origin origin = Origin::line;
    // The setting as written there, for how to clear it: "rclone.socks_proxy",
    // "RCLONE_SFTP_SOCKS_PROXY", "--sftp-socks-proxy"; "" for a store that can't be read.
    std::string setting;
    bool operator==(const Reason&) const = default;
};

// A server's settings, source by source.
struct Sources {
    // rclone's settings, each beating the ones before as rclone had it: grab's store's rclone
    // extras (those bound to the server's login; its proxies there aren't read here: they are the
    // server's route, route()), the section's rclone.<key> lines, the inherited
    // RCLONE_SFTP_<KEY> variables (`environment`; not those 0.20's rclone runs never saw: SSH,
    // PUBKEY and PUBKEY_FILE), common_flags' --sftp-<key> flags, then folder_flags' (after
    // common_flags on 0.19's folder downloads' command line).
    std::vector<std::pair<std::string, std::string>> extras; // keys without "rclone."
    std::vector<std::pair<std::string, std::string>> lines;
    std::function<std::optional<std::string>(const std::string&)> environment;
    std::vector<std::string> common_flags;
    std::vector<std::string> folder_flags;
    std::vector<std::string> ssh_options; // ssh.exe's: -o ProxyCommand, -o ProxyJump, -J (first wins)
};

// Each guard setting a server has, by key, as rclone (or ssh.exe) went by it: where it is set, and
// the kind of reason it is when it keeps grab off (ssh_options' routes have one, but reasons()
// leaves them out).
struct Found {
    std::string where;
    Origin origin = Origin::line;
    std::string setting;
    std::optional<Kind> blocks;
    bool operator==(const Found&) const = default;
};
using Settings = std::map<std::string, Found, std::less<>>;

// grab's store's rclone extras of `server` saved for `login`, as the import kept them, keys
// without "rclone.".
[[nodiscard]] std::vector<std::pair<std::string, std::string>> store_extras(const passwords::Store& store,
                                                                           std::string_view server,
                                                                           const passwords::Binding& login);
// The Sources of a server: its section (`settings`, `connection`'s rclone.<key> lines), `extras`
// and this process's environment.
[[nodiscard]] Sources sources(const RemoteSettings& settings, const server_conf::Connection& connection,
                              std::vector<std::pair<std::string, std::string>> extras);

[[nodiscard]] Settings evaluate(const Sources& sources);
// grab's store exists but can't be read: the extras it may keep (a proxy, an ssh command) are
// unknown, so the server counts as routed ("store", where "grab's store, which can't be read").
void add_unreadable_store(Settings& settings);
// The settings that keep grab off, in key order: not ssh_options' (decision 4).
[[nodiscard]] std::vector<Reason> reasons(const Settings& settings);

// Where a server's proxy comes from (PROXIES.md 3.3).
enum class ProxyFrom {
    none, // directly
    line, // its grab.conf `proxy` line
    kept, // rclone.socks_proxy or rclone.http_proxy in grab's store, saved for its login
};

// The proxy a server is reached through, its password included (memory only, as a Target's).
struct Route {
    proxy::Proxy proxy;
    ProxyFrom from = ProxyFrom::none;
    std::string key; // kept: "socks_proxy" or "http_proxy"
};

// What keeps grab off a server for the proxies its store keeps: the setting, under the key it
// goes by in Settings ("socks_proxy", "http_proxy"; "proxies" for two of them).
struct RouteProblem {
    std::string key;
    Found found;
    bool operator==(const RouteProblem&) const = default;
};

// The proxy's own user@host:port, which its password in the store is bound to.
[[nodiscard]] passwords::Binding proxy_login(const proxy::Proxy& proxy);

// The route of `server`, whose grab.conf section holds `c`: its `proxy` line, with the store's
// proxy_password saved for exactly that proxy (none without a user, or when none is saved for
// it); else the one non-blank rclone.socks_proxy or rclone.http_proxy the store keeps for c's
// login, read in rclone's form (proxy::parse_rclone_socks, parse_rclone_http), its password with
// it; else directly. The line wins over anything the store keeps (a hand edit is the user's
// word); without one, a kept proxy saved for another login, both kinds kept, or one that isn't a
// proxy grab can use is the problem that refuses the server. `any_login`: what an Edit leaves,
// which binds every kept one to the server's new login (server_ops::update_server), so a kept one
// of any login counts.
[[nodiscard]] std::expected<Route, RouteProblem> route(const passwords::Store& store, std::string_view server,
                                                       const server_conf::Connection& c, bool any_login = false);
// The problem into `settings`, as a reason keeping grab off the server.
void add_route_problem(Settings& settings, const RouteProblem& problem);
// text() of that problem alone.
[[nodiscard]] std::string route_text(std::string_view server, const RouteProblem& problem);

// "needs fstat in place of stat (rclone.use_fstat)", "has a SOCKS proxy where grab doesn't read
// one (common_flags' --sftp-socks-proxy)".
[[nodiscard]] std::string describe(const Reason& reason);
// What it means for `server`, as errors, Test, `grab server list`, the window's cards, -v and the
// notice say it: "[Box] needs fstat in place of stat (rclone.use_fstat), which grab can't, so it
// doesn't connect to Box: delete the rclone.use_fstat line in [Box] (grab config)"; a route
// adds "rather than go around it", and a proxy only grab's own setting can give it says so
// ("[Box] has a SOCKS proxy where grab doesn't read one (common_flags' --sftp-socks-proxy), so
// grab doesn't connect to Box rather than go around it: write `proxy = socks5://HOST:PORT` in
// [Box] and take --sftp-socks-proxy out of common_flags (grab config)"). Each setting gets how to
// clear it: a grab.conf line deleted, a flag key edited, a setting the import kept in grab's
// store by removing the server and adding it again, a kept proxy by setting the server's proxy
// with Edit, an inherited variable cleared (it covers every server; for grab alone, in the window
// it starts from), a store that can't be read set aside by saving the server with Edit.
[[nodiscard]] std::string text(std::string_view server, std::span<const Reason> reasons);

} // namespace grab::guard
