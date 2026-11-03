#include "config.hpp"
#include "guard.hpp"
#include "passwords.hpp"
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
    s.extras = {{"socks_proxy", "fake-user:fake-pass@127.0.0.1:9"}, {"set_env", "X=1"}};
    s.lines = {{"use_fstat", "true"}, {"skip_links", "true"}, {"subsystem", "other-sftp"}};
    const auto r = reasons_of(s);
    CHECK(keys_of(r) == std::vector<std::string>{"socks_proxy @ rclone.socks_proxy in grab's store",
                                                 "subsystem @ rclone.subsystem", "use_fstat @ rclone.use_fstat"});
    CHECK(r[0].kind == guard::Kind::route);
    CHECK(r[0].origin == guard::Origin::store);
    CHECK(r[1].kind == guard::Kind::server);
    CHECK(r[1].origin == guard::Origin::line);
    CHECK(r[2].kind == guard::Kind::quirk);
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
    // The command's flags after `--` beat everything.
    auto settings = guard::evaluate(s);
    const std::vector<std::string> run{"--sftp-http-proxy=", "--sftp-ssh", "ssh -W %h:%p jump"};
    guard::add_run_flags(settings, run);
    const auto after = guard::reasons(settings);
    CHECK(keys_of(after) == std::vector<std::string>{"ssh @ --sftp-ssh after --"});
    CHECK(after[0].origin == guard::Origin::run_flags);
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
    s.extras = {{"socks_proxy", "fake-user:fake-secret@127.0.0.1:9"}, {"ssh", "ssh -i C:\\fake-secret-key jump"}};
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
    CHECK(text == "[Box] connects through a SOCKS proxy (rclone.socks_proxy in grab's store) and connects through an ssh "
                  "command (rclone.ssh in grab's store), which grab can't, so it doesn't connect to Box rather than go "
                  "around it: the import kept rclone.socks_proxy and rclone.ssh in grab's store: remove Box and add it "
                  "again (Remove and Add in grab's Settings, or `grab server remove Box` and `grab server add`)");
    // A reason keeps nothing of the value, not even a digest: only where it is set.
    s.extras[0].second = "fake-user:fake-other@127.0.0.1:9";
    CHECK(reasons_of(s) == r);

    // Each source's way out.
    guard::Sources line;
    line.lines = {{"use_fstat", "true"}};
    CHECK(guard::text("Box", reasons_of(line)) ==
          "[Box] needs fstat in place of stat (rclone.use_fstat), which grab can't, so it doesn't connect to Box: delete "
          "the rclone.use_fstat line in [Box] (grab config)");
    guard::Sources flags;
    flags.common_flags = {"--sftp-http-proxy", "fake-proxy:3128"};
    CHECK(guard::text("Main", reasons_of(flags)) ==
          "[Main] connects through an HTTP proxy (common_flags' --sftp-http-proxy), which grab can't, so it doesn't "
          "connect to Main rather than go around it: take --sftp-http-proxy out of common_flags in [Main] (grab config)");
    CHECK(guard::text("Main", reasons_of({}, {{"RCLONE_SFTP_SOCKS_PROXY", "fake-proxy:1080"}})) ==
          "[Main] connects through a SOCKS proxy (RCLONE_SFTP_SOCKS_PROXY), which grab can't, so it doesn't connect to "
          "Main rather than go around it: RCLONE_SFTP_SOCKS_PROXY is set in grab's environment and covers every "
          "server; clear it for grab alone (`set RCLONE_SFTP_SOCKS_PROXY=` in that window before starting grab) or "
          "for good");
    auto run = guard::evaluate({});
    const std::vector<std::string> given{"--sftp-socks-proxy", "fake-proxy:1080"};
    guard::add_run_flags(run, given);
    CHECK(guard::text("Main", guard::reasons(run)).ends_with(": leave --sftp-socks-proxy out after --"));
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
          "[Box] may connect through a proxy or an ssh command (grab's store, which can't be read), which grab can't, so "
          "it doesn't connect to Box rather than go around it: saving Box with Edit (no change needed) sets the "
          "unreadable store aside, and with it any proxy or ssh command kept there: Box then connects directly");
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
    CHECK(keys_of(guard::reasons(guard::evaluate(s))).size() == 3);
}
