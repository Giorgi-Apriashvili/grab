#include "config.hpp"
#include "guard.hpp"
#include "passwords.hpp"
#include "proxy.hpp"
#include "server_conf.hpp"

#include <doctest/doctest.h>

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace grab;

namespace {

using Pairs = std::vector<std::pair<std::string, std::string>>;

// The reasons of a server whose settings come from `s`, with `variables` as its environment.
std::vector<guard::Reason> reasons_of(guard::Sources s, const std::map<std::string, std::string>& variables = {}) {
    s.environment = [&variables](const std::string& name) -> std::optional<std::string> {
        const auto it = variables.find(name);
        if (it == variables.end()) return std::nullopt;
        return it->second;
    };
    return guard::reasons(guard::evaluate(s));
}

std::vector<std::string> keys_of(const std::vector<guard::Reason>& reasons) {
    std::vector<std::string> out;
    for (const auto& r : reasons) out.push_back(r.key + " @ " + r.where);
    return out;
}

} // namespace

TEST_CASE("guard: a route, another server, a certificate, ciphers or quirks keep grab off (phase 8)") {
    guard::Sources s;
    // The store's proxy is the server's route now (guard::route, PROXIES.md decision 4), never a
    // reason; its ssh command still is.
    s.extras = {{"socks_proxy", "fake-user:fake-pass@127.0.0.1:9"}, {"ssh", "fake-ssh jump"}, {"set_env", "X=1"}};
    s.lines = {{"use_fstat", "true"}, {"skip_links", "true"}, {"subsystem", "other-sftp"}};
    const auto r = reasons_of(s);
    REQUIRE(keys_of(r) == std::vector<std::string>{"ssh @ rclone.ssh in grab's store", "subsystem @ rclone.subsystem",
                                                   "use_fstat @ rclone.use_fstat"});
    CHECK(r[0].kind == guard::Kind::route);
    CHECK(r[0].origin == guard::Origin::store);
    CHECK(r[1].kind == guard::Kind::server);
    CHECK(r[1].origin == guard::Origin::line);
    CHECK(r[2].kind == guard::Kind::quirk);
    s.extras = {{"socks_proxy", "fake-user:fake-pass@127.0.0.1:9"}, {"http_proxy", "http://fake-proxy:3128"}};
    s.lines.clear();
    CHECK(reasons_of(s).empty());
    // Values decide, not names: these leave grab's client as it is.
    s.extras = {{"http_proxy", ""}, {"ssh", "  "}};
    s.lines = {{"subsystem", "sftp"}, {"use_fstat", "false"}, {"disable_concurrent_reads", "nonsense"},
               {"use_insecure_cipher", "0"}, {"ciphers", "aes256-ctr 3des-cbc"}, {"key_exchange", "curve25519-sha256"},
               {"macs", "hmac-sha2-256,hmac-sha1"}, {"host_key_algorithms", "ssh-ed25519"}, {"pubkey_file", ""}};
    s.ssh_options = {"-o", "ProxyCommand none", "-oProxyJump=none", "-o", "ServerAliveInterval=30"};
    CHECK(reasons_of(s).empty());
    // Lists that share nothing with grab's offer, a certificate, insecure ciphers.
    s.lines = {{"ciphers", "aes128-cbc 3des-cbc"}, {"macs", "hmac-sha1"}, {"key_exchange", "diffie-hellman-group1-sha1"},
               {"host_key_algorithms", "ssh-dss"}, {"pubkey_file", "C:\\keys\\id-cert.pub"}, {"use_insecure_cipher", "True"},
               {"disable_concurrent_reads", "t"}};
    s.ssh_options.clear();
    CHECK(keys_of(reasons_of(s)) ==
          std::vector<std::string>{"ciphers @ rclone.ciphers", "disable_concurrent_reads @ rclone.disable_concurrent_reads",
                                   "host_key_algorithms @ rclone.host_key_algorithms", "key_exchange @ rclone.key_exchange",
                                   "macs @ rclone.macs", "pubkey_file @ rclone.pubkey_file",
                                   "use_insecure_cipher @ rclone.use_insecure_cipher"});
}

TEST_CASE("guard: each source, and the one rclone went by when several set a key") {
    guard::Sources s;
    // The store's extra, then the grab.conf line, then the environment, then common_flags.
    s.extras = {{"socks_proxy", "127.0.0.1:9"}};
    s.lines = {{"socks_proxy", ""}};
    CHECK(reasons_of(s).empty()); // the line, blank, beats the store's
    s.lines.clear();
    CHECK(reasons_of(s).empty()); // the store's alone is the server's route (guard::route)
    const auto inherited = reasons_of(s, {{"RCLONE_SFTP_SOCKS_PROXY", "127.0.0.1:10"}});
    CHECK(keys_of(inherited) == std::vector<std::string>{"socks_proxy @ RCLONE_SFTP_SOCKS_PROXY"});
    CHECK(inherited[0].origin == guard::Origin::environment);
    CHECK(inherited[0].setting == "RCLONE_SFTP_SOCKS_PROXY");
    s.common_flags = {"-P", "--sftp-socks-proxy=127.0.0.1:11", "--sftp-chunk-size", "255Ki"};
    const auto flagged = reasons_of(s, {{"RCLONE_SFTP_SOCKS_PROXY", "127.0.0.1:10"}});
    CHECK(keys_of(flagged) == std::vector<std::string>{"socks_proxy @ common_flags' --sftp-socks-proxy"});
    CHECK(flagged[0].origin == guard::Origin::common_flags);
    CHECK(flagged[0].setting == "--sftp-socks-proxy");
    // A boolean flag alone is true; "=false" turns an earlier true off.
    s = {};
    s.common_flags = {"--sftp-use-fstat"};
    CHECK(keys_of(reasons_of(s)) == std::vector<std::string>{"use_fstat @ common_flags' --sftp-use-fstat"});
    s.lines = {{"disable_concurrent_reads", "true"}};
    s.common_flags = {"--sftp-disable-concurrent-reads=false", "--sftp-http-proxy", "proxy.example:3128"};
    CHECK(keys_of(reasons_of(s)) == std::vector<std::string>{"http_proxy @ common_flags' --sftp-http-proxy"});
    // folder_flags, after common_flags on grab 0.19's folder downloads: its own place to clear.
    s = {};
    s.common_flags = {"--sftp-socks-proxy", "fake-user:fake-pass@127.0.0.1:1"};
    s.folder_flags = {"--transfers", "8", "--sftp-socks-proxy=", "--sftp-ssh", "fake-ssh -W %h:%p"};
    const auto folder = reasons_of(s);
    CHECK(keys_of(folder) == std::vector<std::string>{"ssh @ folder_flags' --sftp-ssh"});
    REQUIRE(folder.size() == 1);
    CHECK(folder[0].origin == guard::Origin::folder_flags);
    CHECK(guard::text("Box", folder).ends_with("take --sftp-ssh out of folder_flags in [Box] (grab config)"));
    CHECK(guard::text("Box", folder).find("fake-") == std::string::npos);
}

TEST_CASE("guard: inherited variables 0.20's rclone never saw don't count; the others cover every server") {
    // RCLONE_SFTP_SSH, _PUBKEY and _PUBKEY_FILE were left out of every rclone run (they would have
    // replaced the server's own settings): the exception list, now the guard's own.
    guard::Sources s;
    CHECK(reasons_of(s, {{"RCLONE_SFTP_SSH", "ssh jump"}, {"RCLONE_SFTP_PUBKEY", "x"}, {"RCLONE_SFTP_PUBKEY_FILE", "x"}})
              .empty());
    CHECK(keys_of(reasons_of(s, {{"RCLONE_SFTP_USE_FSTAT", "1"}, {"RCLONE_SFTP_HTTP_PROXY", "p:1"},
                                 {"RCLONE_SFTP_SUBSYSTEM", "other"}, {"RCLONE_SFTP_CIPHERS", "3des-cbc"}})) ==
          std::vector<std::string>{"ciphers @ RCLONE_SFTP_CIPHERS", "http_proxy @ RCLONE_SFTP_HTTP_PROXY",
                                   "subsystem @ RCLONE_SFTP_SUBSYSTEM", "use_fstat @ RCLONE_SFTP_USE_FSTAT"});
    // A blank one is no setting.
    CHECK(reasons_of(s, {{"RCLONE_SFTP_SOCKS_PROXY", ""}}).empty());
}

TEST_CASE("guard: ssh_options' ProxyCommand, ProxyJump and -J are read but never a reason (decision 4)") {
    // They only ever routed ssh.exe's searches, never rclone's downloads: grab connects directly,
    // and a one-time note says so.
    guard::Sources s;
    s.ssh_options = {"-4", "-o", "proxycommand=ssh -W %h:%p jump", "-o", "ProxyCommand none"};
    auto settings = guard::evaluate(s);
    REQUIRE(settings.contains("ProxyCommand"));
    CHECK(settings.at("ProxyCommand").origin == guard::Origin::ssh_options);
    CHECK(settings.at("ProxyCommand").where == "ssh_options' -o ProxyCommand");
    CHECK(settings.at("ProxyCommand").blocks == guard::Kind::route); // the first one given, as OpenSSH
    CHECK(guard::reasons(settings).empty());
    s.ssh_options = {"-J", "fake-user@jump.example"};
    settings = guard::evaluate(s);
    CHECK(settings.at("ProxyJump").where == "ssh_options' -J");
    CHECK(guard::reasons(settings).empty());
    s.ssh_options = {"-Jjump.example", "-o", "ProxyJump=other"};
    CHECK(guard::evaluate(s).at("ProxyJump").where == "ssh_options' -J");
    CHECK(reasons_of(s).empty());
    // Beside a reason of another source, only that one is said.
    s.lines = {{"socks_proxy", "127.0.0.1:9"}};
    CHECK(keys_of(reasons_of(s)) == std::vector<std::string>{"socks_proxy @ rclone.socks_proxy"});
}

TEST_CASE("guard: texts name settings and how to clear them, never a value or engine = rclone") {
    guard::Sources s;
    s.extras = {{"pubkey", "fake-secret-cert@127.0.0.1"}, {"ssh", "ssh -i C:\\fake-secret-key jump"}};
    s.ssh_options = {"-o", "ProxyJump=fake-secret-jump"};
    const auto r = reasons_of(s);
    REQUIRE(r.size() == 2); // ssh_options' jump host isn't one
    const auto text = guard::text("Box", r);
    for (const auto& t : {text, guard::describe(r[0])}) {
        CHECK(t.find("fake-") == std::string::npos);
        CHECK(t.find("127.0.0.1") == std::string::npos);
        CHECK(t.find("engine = rclone") == std::string::npos);
    }
    // The store's settings: one way out for all of them, to remove the server and add it again.
    CHECK(text == "[Box] logs in with a certificate (rclone.pubkey in grab's store) and connects through an ssh command "
                  "(rclone.ssh in grab's store), which grab can't, so it doesn't connect to Box rather than go around "
                  "it: the import kept rclone.pubkey and rclone.ssh in grab's store: remove Box and add it again (Remove "
                  "and Add in grab's Settings, or `grab server remove Box` and `grab server add`)");
    // A reason keeps nothing of the value, not even a digest: only where it is set.
    s.extras[0].second = "fake-other-cert@127.0.0.1";
    CHECK(reasons_of(s) == r);

    // Each source's way out.
    guard::Sources line;
    line.lines = {{"use_fstat", "true"}};
    CHECK(guard::text("Box", reasons_of(line)) ==
          "[Box] needs fstat in place of stat (rclone.use_fstat), which grab can't, so it doesn't connect to Box: delete "
          "the rclone.use_fstat line in [Box] (grab config)");
    // A proxy where grab doesn't read one: grab's own setting instead (PROXIES.md 2), never quoted.
    guard::Sources flags;
    flags.common_flags = {"--sftp-http-proxy", "fake-proxy:3128"};
    CHECK(guard::text("Main", reasons_of(flags)) ==
          "[Main] has an HTTP proxy where grab doesn't read one (common_flags' --sftp-http-proxy), so grab doesn't "
          "connect to Main rather than go around it: write `proxy = http://HOST:PORT` in [Main] and take "
          "--sftp-http-proxy out of common_flags (grab config)");
    flags.common_flags.clear();
    flags.folder_flags = {"--sftp-socks-proxy=fake-proxy:1080"};
    CHECK(guard::text("Main", reasons_of(flags)) ==
          "[Main] has a SOCKS proxy where grab doesn't read one (folder_flags' --sftp-socks-proxy), so grab doesn't "
          "connect to Main rather than go around it: write `proxy = socks5://HOST:PORT` in [Main] and take "
          "--sftp-socks-proxy out of folder_flags (grab config)");
    flags.folder_flags.clear();
    flags.lines = {{"socks_proxy", "fake-proxy:1080"}};
    CHECK(guard::text("Main", reasons_of(flags)) ==
          "[Main] has a SOCKS proxy where grab doesn't read one (rclone.socks_proxy), so grab doesn't connect to Main "
          "rather than go around it: write `proxy = socks5://HOST:PORT` in [Main] and delete the rclone.socks_proxy "
          "line (grab config)");
    CHECK(guard::text("Main", reasons_of({}, {{"RCLONE_SFTP_SOCKS_PROXY", "fake-proxy:1080"}})) ==
          "[Main] has a SOCKS proxy in a variable grab doesn't follow (RCLONE_SFTP_SOCKS_PROXY), so grab doesn't "
          "connect to Main rather than go around it: grab doesn't follow proxy variables: set the proxy per server "
          "(`proxy = socks5://HOST:PORT` in [Main]) and clear RCLONE_SFTP_SOCKS_PROXY for grab (`set "
          "RCLONE_SFTP_SOCKS_PROXY=` in that window before starting grab) or for good");
    // Beside a setting grab can't, that one says so.
    flags.lines = {{"use_fstat", "true"}};
    flags.common_flags = {"--sftp-socks-proxy", "fake-proxy:1080"};
    CHECK(guard::text("Main", reasons_of(flags)) ==
          "[Main] has a SOCKS proxy where grab doesn't read one (common_flags' --sftp-socks-proxy) and needs fstat in "
          "place of stat (rclone.use_fstat), which grab can't, so it doesn't connect to Main rather than go around it: "
          "write `proxy = socks5://HOST:PORT` in [Main] and take --sftp-socks-proxy out of common_flags (grab config); "
          "delete the rclone.use_fstat line in [Main] (grab config)");
}

TEST_CASE("guard: grab's store that can't be read counts as a route, set aside by an Edit") {
    guard::Sources s;
    auto settings = guard::evaluate(s);
    CHECK(guard::reasons(settings).empty());
    guard::add_unreadable_store(settings);
    const auto r = guard::reasons(settings);
    REQUIRE(r.size() == 1);
    CHECK(r[0].kind == guard::Kind::route);
    CHECK(r[0].origin == guard::Origin::unreadable);
    CHECK(guard::describe(r[0]) == "may connect through a proxy or an ssh command (grab's store, which can't be read)");
    CHECK(guard::text("Box", r) ==
          "[Box] may connect through a proxy or an ssh command (grab's store, which can't be read), so grab doesn't "
          "connect to Box rather than go around it: saving Box with Edit (no change needed) sets the unreadable store "
          "aside, and with it any proxy or ssh command kept there: Box then connects directly, or through the proxy "
          "its grab.conf section gives it");
}

TEST_CASE("guard: a server's sources from its section, the store's extras for its login and the environment") {
    passwords::Store store;
    const passwords::Binding login{.user = "u", .host = "box.example", .port = 23};
    store["Box"]["rclone.socks_proxy"] = passwords::Secret{.binding = login, .value = "127.0.0.1:9"};
    store["Box"]["rclone.http_proxy"] =
        passwords::Secret{.binding = passwords::Binding{.user = "u", .host = "old.example", .port = 23}, .value = "p:1"};
    store["Box"]["password"] = passwords::Secret{.binding = login, .value = "fake-pw"};
    store["Other"]["rclone.ssh"] = passwords::Secret{.binding = login, .value = "ssh x"};
    // Only rclone extras, only for exactly that login (as the import kept them).
    CHECK(guard::store_extras(store, "Box", login) == Pairs{{"socks_proxy", "127.0.0.1:9"}});
    RemoteSettings settings;
    settings.name = "Box";
    settings.common_flags = {"--sftp-use-fstat"};
    server_conf::Connection c;
    c.rclone = {{"subsystem", "x"}};
    const auto s = guard::sources(settings, c, guard::store_extras(store, "Box", login));
    CHECK(s.lines == Pairs{{"subsystem", "x"}});
    CHECK(s.common_flags == settings.common_flags);
    // The store's proxy isn't one of them: it is the server's route.
    CHECK(keys_of(guard::reasons(guard::evaluate(s))) ==
          std::vector<std::string>{"subsystem @ rclone.subsystem", "use_fstat @ common_flags' --sftp-use-fstat"});
}

namespace {

server_conf::Connection login_of(std::string host, std::string user = "u", int port = 23) {
    server_conf::Connection c;
    c.host = std::move(host);
    c.user = std::move(user);
    c.port = port;
    return c;
}

passwords::Secret kept(std::string value, const server_conf::Connection& at) {
    return passwords::Secret{.binding = {.user = at.user, .host = at.host, .port = at.port}, .value = std::move(value)};
}

} // namespace

TEST_CASE("guard: a server's route, its proxy line first, else the one its store keeps for its login (PROXIES.md 3.3)") {
    const auto box = login_of("box.example");
    const passwords::Binding alice_at{.user = "alice", .host = "127.0.0.1", .port = 9050};
    // Directly: nothing set.
    auto r = guard::route({}, "Box", box);
    REQUIRE(r);
    CHECK(r->from == guard::ProxyFrom::none);
    CHECK(r->proxy == proxy::Proxy{});

    // The line, with the password saved for that very proxy and user.
    auto line = box;
    line.proxy = proxy::Proxy{.kind = proxy::Kind::socks5, .host = "127.0.0.1", .port = 9050, .user = "alice", .password = {}};
    passwords::Store store;
    store["Box"]["proxy_password"] = passwords::Secret{.binding = alice_at, .value = "fake-proxy-pw"};
    r = guard::route(store, "Box", line);
    REQUIRE(r);
    CHECK(r->from == guard::ProxyFrom::line);
    CHECK(r->proxy.kind == proxy::Kind::socks5);
    CHECK(r->proxy.user == "alice");
    CHECK(r->proxy.password == "fake-proxy-pw");
    CHECK(guard::proxy_login(r->proxy) == alice_at);
    // Another proxy, port or user written by hand: the password stays with the one it was typed for.
    for (auto change : {+[](proxy::Proxy& p) { p.port = 9051; }, +[](proxy::Proxy& p) { p.host = "127.0.0.2"; },
                        +[](proxy::Proxy& p) { p.user = "bob"; }}) {
        auto moved = line;
        change(moved.proxy);
        const auto other = guard::route(store, "Box", moved);
        REQUIRE(other);
        CHECK(other->from == guard::ProxyFrom::line);
        CHECK(other->proxy.password.empty());
    }
    // The server's own login changed: the proxy's password is the proxy's, not the login's.
    auto relogged = line;
    relogged.port = 2222;
    CHECK(guard::route(store, "Box", relogged).value_or(guard::Route{}).proxy.password == "fake-proxy-pw");
    // No user: nothing is sent, nothing looked up.
    auto anonymous = line;
    anonymous.proxy.user.clear();
    CHECK(guard::route(store, "Box", anonymous).value_or(guard::Route{}).proxy.password.empty());

    // Kept in rclone's form for its login (the import's, decision 4): its route again.
    store["Box"]["rclone.socks_proxy"] = kept("alice:fake-kept-pw@127.0.0.1:9050", box);
    r = guard::route(store, "Box", box);
    REQUIRE(r);
    CHECK(r->from == guard::ProxyFrom::kept);
    CHECK(r->key == "socks_proxy");
    CHECK(r->proxy == proxy::Proxy{.kind = proxy::Kind::socks5, .host = "127.0.0.1", .port = 9050, .user = "alice",
                                   .password = "fake-kept-pw"});
    // The line wins over it (a hand edit is the user's word).
    r = guard::route(store, "Box", line);
    REQUIRE(r);
    CHECK(r->from == guard::ProxyFrom::line);
    CHECK(r->proxy.port == 9050);
    CHECK(r->proxy.password == "fake-proxy-pw");
    store["Box"].erase("rclone.socks_proxy");
    store["Box"]["rclone.http_proxy"] = kept("http://al%40ice:fake%3Apw@proxy.example:3128/", box);
    r = guard::route(store, "Box", box);
    REQUIRE(r);
    CHECK(r->proxy == proxy::Proxy{.kind = proxy::Kind::http, .host = "proxy.example", .port = 3128, .user = "al@ice",
                                   .password = "fake:pw"});
    // A blank one is none, as rclone read it.
    store["Box"]["rclone.http_proxy"] = kept("  ", box);
    CHECK(guard::route(store, "Box", box).value_or(guard::Route{.proxy = {}, .from = guard::ProxyFrom::line, .key = {}}).from ==
          guard::ProxyFrom::none);
}

TEST_CASE("guard: a kept proxy saved for another login, two of them, or one grab can't use refuse the server") {
    const auto box = login_of("box.example");
    const auto old = login_of("old.example");
    passwords::Store store;
    // Saved for another login: 0.21-0.25 skipped it and connected directly; now it refuses.
    store["Box"]["rclone.socks_proxy"] = kept("fake-user:fake-pass@127.0.0.1:9050", old);
    auto r = guard::route(store, "Box", box);
    REQUIRE_FALSE(r);
    CHECK(r.error().key == "socks_proxy");
    CHECK(r.error().found.origin == guard::Origin::store);
    CHECK(r.error().found.blocks == guard::Kind::route);
    CHECK(guard::route_text("Box", r.error()) ==
          "[Box] has a SOCKS proxy (rclone.socks_proxy in grab's store, saved for another login), so grab doesn't "
          "connect to Box rather than go around it: Edit Box and set its proxy again, or choose Directly");
    // What an Edit leaves binds it to the new login: it counts there.
    r = guard::route(store, "Box", box, true);
    REQUIRE(r);
    CHECK(r->from == guard::ProxyFrom::kept);
    CHECK(r->proxy.password == "fake-pass");
    // A line wins over it.
    auto line = box;
    line.proxy = proxy::Proxy{.kind = proxy::Kind::http, .host = "proxy.example", .port = 3128, .user = {}, .password = {}};
    CHECK(guard::route(store, "Box", line).has_value());

    // Two of them, whatever their logins.
    store["Box"]["rclone.http_proxy"] = kept("http://proxy.example:3128", box);
    for (const bool any : {false, true}) {
        r = guard::route(store, "Box", box, any);
        REQUIRE_FALSE(r);
        CHECK(r.error().key == "proxies");
        CHECK(guard::route_text("Box", r.error()) ==
              "[Box] has both a SOCKS and an HTTP proxy (rclone.socks_proxy and rclone.http_proxy in grab's store), so "
              "grab doesn't connect to Box rather than go around it: Edit Box and choose one of them, or Directly");
    }
    CHECK(guard::route(store, "Box", line).has_value());
    store["Box"].erase("rclone.socks_proxy");

    // One grab can't use: the reason, never the value.
    store["Box"]["rclone.http_proxy"] = kept("https://fake-user:fake-pass@proxy.example:3128", box);
    r = guard::route(store, "Box", box);
    REQUIRE_FALSE(r);
    const auto said = guard::route_text("Box", r.error());
    CHECK(said == "[Box] has an HTTP proxy (rclone.http_proxy in grab's store, which grab can't use: grab can't use HTTPS "
                  "proxies yet: use http://), so grab doesn't connect to Box rather than go around it: Edit Box and set "
                  "its proxy again, or choose Directly");
    store["Box"]["rclone.http_proxy"] = kept("fake-user:fake-pass@proxy.example", box);
    r = guard::route(store, "Box", box);
    REQUIRE_FALSE(r);
    CHECK(guard::route_text("Box", r.error()).find("fake-") == std::string::npos);

    // A problem joins the server's other reasons; one that refuses there already says enough.
    guard::Settings settings;
    guard::add_route_problem(settings, r.error());
    CHECK(guard::reasons(settings).size() == 1);
    guard::Sources flagged;
    flagged.common_flags = {"--sftp-http-proxy", "fake-proxy:3128"};
    settings = guard::evaluate(flagged);
    guard::add_route_problem(settings, r.error());
    const auto both = guard::reasons(settings);
    REQUIRE(both.size() == 1);
    CHECK(both[0].origin == guard::Origin::common_flags);
}
