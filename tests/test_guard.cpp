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

TEST_CASE("guard: a route, another server, a certificate, ciphers or quirks keep grab's client off (phase 8)") {
    guard::Sources s;
    s.extras = {{"socks_proxy", "fake-user:fake-pass@127.0.0.1:9"}, {"set_env", "X=1"}};
    s.lines = {{"use_fstat", "true"}, {"skip_links", "true"}, {"subsystem", "other-sftp"}};
    const auto r = reasons_of(s);
    CHECK(keys_of(r) == std::vector<std::string>{"socks_proxy @ rclone.socks_proxy", "subsystem @ rclone.subsystem",
                                                 "use_fstat @ rclone.use_fstat"});
    CHECK(r[0].kind == guard::Kind::route);
    CHECK(r[1].kind == guard::Kind::server);
    CHECK(r[2].kind == guard::Kind::quirk);
    CHECK(guard::routed(r));
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
    CHECK_FALSE(guard::routed(reasons_of(s)));
}

TEST_CASE("guard: each source, and the one rclone goes by when several set a key") {
    guard::Sources s;
    // The store's extra, then the grab.conf line, then the environment, then common_flags.
    s.extras = {{"socks_proxy", "127.0.0.1:9"}};
    s.lines = {{"socks_proxy", ""}};
    CHECK(reasons_of(s).empty()); // the line, blank, beats the store's
    s.lines.clear();
    CHECK(keys_of(reasons_of(s, {{"RCLONE_SFTP_SOCKS_PROXY", "127.0.0.1:10"}})) ==
          std::vector<std::string>{"socks_proxy @ RCLONE_SFTP_SOCKS_PROXY"});
    s.common_flags = {"-P", "--sftp-socks-proxy=127.0.0.1:11", "--sftp-chunk-size", "255Ki"};
    CHECK(keys_of(reasons_of(s, {{"RCLONE_SFTP_SOCKS_PROXY", "127.0.0.1:10"}})) ==
          std::vector<std::string>{"socks_proxy @ common_flags' --sftp-socks-proxy"});
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
    CHECK(keys_of(guard::reasons(settings)) == std::vector<std::string>{"ssh @ --sftp-ssh after --"});
    // Variables rclone never sees don't count: RCLONE_SFTP_SSH and the certificate ones are dropped
    // for its runs (rclone_flags::kept_variable); the others do.
    s = {};
    CHECK(reasons_of(s, {{"RCLONE_SFTP_SSH", "ssh jump"}, {"RCLONE_SFTP_PUBKEY_FILE", "x"}}).empty());
    CHECK(keys_of(reasons_of(s, {{"RCLONE_SFTP_USE_FSTAT", "1"}, {"RCLONE_SFTP_HTTP_PROXY", "p:1"}})) ==
          std::vector<std::string>{"http_proxy @ RCLONE_SFTP_HTTP_PROXY", "use_fstat @ RCLONE_SFTP_USE_FSTAT"});
    // ssh_options: ProxyCommand, ProxyJump and -J, any case, the first one given (as OpenSSH).
    s = {};
    s.ssh_options = {"-4", "-o", "proxycommand=ssh -W %h:%p jump", "-o", "ProxyCommand none"};
    auto r = reasons_of(s);
    CHECK(keys_of(r) == std::vector<std::string>{"ProxyCommand @ ssh_options' -o ProxyCommand"});
    CHECK(r[0].ssh_only);
    CHECK_FALSE(guard::routed(r)); // only ssh.exe went through it: rclone's listing isn't needed
    s.ssh_options = {"-J", "fake-user@jump.example"};
    CHECK(keys_of(reasons_of(s)) == std::vector<std::string>{"ProxyJump @ ssh_options' -J"});
    s.ssh_options = {"-Jjump.example"};
    CHECK(keys_of(reasons_of(s)) == std::vector<std::string>{"ProxyJump @ ssh_options' -J"});
}

TEST_CASE("guard: texts name settings, never a value; on engine = grab they say to set engine = rclone") {
    guard::Sources s;
    s.extras = {{"socks_proxy", "fake-user:fake-secret@127.0.0.1:9"}, {"ssh", "ssh -i C:\\fake-secret-key jump"}};
    s.ssh_options = {"-o", "ProxyJump=fake-secret-jump"};
    const auto r = reasons_of(s);
    REQUIRE(r.size() == 3);
    const auto on_rclone = guard::text("Box", r, Engine::rclone);
    const auto on_grab = guard::text("Box", r, Engine::grab);
    for (const auto& t : {on_rclone, on_grab, guard::describe(r[0])}) {
        CHECK(t.find("fake-") == std::string::npos);
        CHECK(t.find("127.0.0.1") == std::string::npos);
    }
    CHECK(on_rclone.starts_with("[Box] connects through a jump host (ssh_options' -o ProxyJump), connects through a SOCKS "
                                "proxy (rclone.socks_proxy) and connects through an ssh command (rclone.ssh), which "
                                "grab's own client can't: its downloads go through rclone, and its searches and lists by "
                                "rclone's listing; previews, Play and Verify are off for it"));
    CHECK(on_rclone.find("checks host keys itself, not against grab's known_hosts") != std::string::npos);
    CHECK(on_rclone.find("rclone's downloads never went through them") != std::string::npos);
    CHECK(on_grab.find("set engine = rclone in [Box] (grab config)") != std::string::npos);
    // A reason keeps nothing of the value, not even a digest: only where it is set.
    s.extras[0].second = "fake-user:fake-other@127.0.0.1:9";
    CHECK(reasons_of(s) == r);
}

TEST_CASE("guard: grab's store that can't be read counts as a route") {
    guard::Sources s;
    auto settings = guard::evaluate(s);
    CHECK(guard::reasons(settings).empty());
    guard::add_unreadable_store(settings);
    const auto r = guard::reasons(settings);
    REQUIRE(r.size() == 1);
    CHECK(r[0].kind == guard::Kind::route);
    CHECK(guard::routed(r));
    CHECK(guard::describe(r[0]) == "may connect through a proxy or an ssh command (grab's store, which can't be read)");
    CHECK(guard::text("Box", r, Engine::rclone).find("its searches and lists by rclone's listing") != std::string::npos);
    CHECK(guard::text("Box", r, Engine::grab).find("sets the unreadable file aside") != std::string::npos);
}

TEST_CASE("guard: a server's sources from its section, the store's extras for its login and the environment") {
    passwords::Store store;
    const passwords::Binding login{.user = "u", .host = "box.example", .port = 23};
    store["Box"]["rclone.socks_proxy"] = passwords::Secret{.binding = login, .value = "127.0.0.1:9"};
    store["Box"]["rclone.http_proxy"] =
        passwords::Secret{.binding = passwords::Binding{.user = "u", .host = "old.example", .port = 23}, .value = "p:1"};
    store["Box"]["password"] = passwords::Secret{.binding = login, .value = "fake-pw"};
    store["Other"]["rclone.ssh"] = passwords::Secret{.binding = login, .value = "ssh x"};
    // Only rclone extras, only for exactly that login (as rclone_env gives them to rclone).
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
