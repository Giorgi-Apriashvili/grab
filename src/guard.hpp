#pragma once

// The servers grab's own SSH client can't serve as their settings say (RCLONE-REPLACEMENT.md,
// phase 8 step 4): a SOCKS or HTTP proxy, an ssh command, a ProxyCommand or ProxyJump (grab's
// client would connect directly, past the route set up for it), another SFTP subsystem or server
// command (it would see another server's files), a certificate login, insecure ciphers or
// algorithm lists that share nothing with grab's offer, and the switches for servers that need one
// read at a time or fstat. grab's client refuses such a server with the reason, and it stays on
// rclone. Values decide, never a key's presence ("subsystem = sftp", "use_fstat = false" and
// "ProxyCommand none" are fine), and no reason carries a value: a proxy can hold a user:password.
// Pure functions.

#include "config.hpp"
#include "passwords.hpp"
#include "server_conf.hpp"

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
    route,       // a proxy, an ssh command, a ProxyCommand or ProxyJump
    server,      // another SFTP subsystem or server command
    certificate, // pubkey, pubkey_file
    cipher,      // use_insecure_cipher; ciphers, key exchanges, MACs or host key types grab doesn't offer
    quirk,       // disable_concurrent_reads, use_fstat
};

// One setting that keeps grab's own client off a server.
struct Reason {
    Kind kind = Kind::route;
    std::string key;   // rclone's option ("socks_proxy"), or ssh's ("ProxyCommand", "ProxyJump")
    // Where it is set: "rclone.socks_proxy" (a grab.conf line, or grab's store, where the import
    // keeps those that can hold a password), "RCLONE_SFTP_SOCKS_PROXY", "common_flags'
    // --sftp-socks-proxy", "--sftp-socks-proxy after --", "ssh_options' -o ProxyCommand".
    std::string where;
    bool ssh_only = false; // ssh_options': only ssh.exe (ssh + find) ever went through it, not rclone
    bool operator==(const Reason&) const = default;
};

// A server's settings, source by source.
struct Sources {
    // rclone's settings, each beating the ones before as rclone has it: grab's store's rclone
    // extras (those bound to the server's login), the section's rclone.<key> lines, the inherited
    // RCLONE_SFTP_<KEY> variables (`environment`; only those rclone_flags::kept_variable lets
    // reach rclone), common_flags' --sftp-<key> flags.
    std::vector<std::pair<std::string, std::string>> extras; // keys without "rclone."
    std::vector<std::pair<std::string, std::string>> lines;
    std::function<std::optional<std::string>(const std::string&)> environment;
    std::vector<std::string> common_flags;
    std::vector<std::string> ssh_options; // ssh.exe's: -o ProxyCommand, -o ProxyJump, -J (first wins)
};

// Each guard setting a server has, by key, as rclone (or ssh.exe) goes by it: where it is set, and
// the kind of reason it is when it keeps grab's client off.
struct Found {
    std::string where;
    bool ssh_only = false;
    std::optional<Kind> blocks;
    bool operator==(const Found&) const = default;
};
using Settings = std::map<std::string, Found, std::less<>>;

// grab's store's rclone extras of `server` saved for `login` (as engine::rclone_env gives them to
// rclone), keys without "rclone.".
[[nodiscard]] std::vector<std::pair<std::string, std::string>> store_extras(const passwords::Store& store,
                                                                           std::string_view server,
                                                                           const passwords::Binding& login);
// The Sources of a server: its section (`settings`, `connection`'s rclone.<key> lines), `extras`
// and this process's environment.
[[nodiscard]] Sources sources(const RemoteSettings& settings, const server_conf::Connection& connection,
                              std::vector<std::pair<std::string, std::string>> extras);

[[nodiscard]] Settings evaluate(const Sources& sources);
// grab's store exists but can't be read: the extras it may keep (a proxy, an ssh command) are
// unknown, so the server counts as routed ("store", where "grab's store, which can't be read"),
// and rclone isn't started for it either (engine::rclone_env).
void add_unreadable_store(Settings& settings);
// A command's connection flags after `--` on top: they beat everything before them.
void add_run_flags(Settings& settings, std::span<const std::string> run_flags);
// The settings that keep grab's client off, in key order.
[[nodiscard]] std::vector<Reason> reasons(const Settings& settings);
// Whether one of them is a route rclone takes (ssh_options' are ssh.exe's only): rclone's listing
// then serves the server's searches and lists, never ssh.exe find, which would ignore it.
[[nodiscard]] bool routed(std::span<const Reason> reasons);

// "connects through a SOCKS proxy (rclone.socks_proxy)".
[[nodiscard]] std::string describe(const Reason& reason);
// What it means for `server` on `engine`, as errors, Test, `grab server list`, -v and the notice
// say it: "[Box] connects through a SOCKS proxy (rclone.socks_proxy), which grab's own client
// can't: its downloads go through rclone, and its searches and lists by rclone's listing;
// previews, Play and Verify are off for it". On engine = grab it says to set engine = rclone; with
// an unreadable store, that rclone isn't started either.
[[nodiscard]] std::string text(std::string_view server, std::span<const Reason> reasons, Engine engine);

} // namespace grab::guard
