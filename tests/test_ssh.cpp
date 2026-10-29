#include <doctest/doctest.h>

#include "config.hpp"
#include "conn_budget.hpp"
#include "fake_command.hpp"
#include "live_import.hpp"
#include "passwords.hpp"
#include "server_conf.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"
#include "ssh_target.hpp"
#include "util.hpp"

#include <libssh2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace grab;

TEST_CASE("libssh2 is built in, on OpenSSL") {
    REQUIRE(libssh2_init(0) == 0);
    CHECK(std::string_view(libssh2_version(0)).starts_with("1.11"));
    CHECK(libssh2_crypto_engine() == libssh2_openssl);
    libssh2_exit();
}

TEST_CASE("ssh: host key algorithms come from the keys pinned for that host and port") {
    const std::string known = "# grab\n"
                              "[box.example]:23 ssh-ed25519 AAAAC3Nza\n"
                              "[box.example]:23 ecdsa-sha2-nistp521 AAAAE2Vj\r\n"
                              "[box.example]:23 ssh-rsa AAAAB3Nz\n"
                              "[other.example]:23 ssh-ed25519 AAAAC3Nzb\n"
                              "plain.example,10.0.0.5 ecdsa-sha2-nistp256 AAAAE2Vk\n"
                              "[box.example]:2222 ssh-ed25519 AAAAC3Nzc\n";
    CHECK(ssh::pinned_algorithms(known, "box.example", 23) == "ssh-ed25519,ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256");
    CHECK(ssh::pinned_algorithms(known, "plain.example", 22) == "ecdsa-sha2-nistp256");
    CHECK(ssh::pinned_algorithms(known, "10.0.0.5", 22) == "ecdsa-sha2-nistp256");
    CHECK(ssh::pinned_algorithms(known, "box.example", 22).empty()); // a port 22 key only as "host"
    CHECK(ssh::pinned_algorithms(known, "nowhere.example", 23).empty());
}

namespace {

// Keys made for these tests (ssh-keygen), with the fingerprints ssh-keygen -l shows for them.
struct TestKey {
    std::string_view type, key, fingerprint;
};
constexpr TestKey rsa_key{
    .type = "ssh-rsa",
    .key = "AAAAB3NzaC1yc2EAAAADAQABAAABAQDmFsW5RTt2X35yH7xv4oA7MYoFKISiTmOb1x49ZNAnFCx9y/MIKPlviZrlz7QHA9vN2gAUldof891BiQBRee/"
           "h1+HQRTvOWM9JRl1bns3aL5eGEfS8BX4X7ztB9RPdTR4ZUeiBGlxC6CQE2YzRn9unHU6ernpRPSXRp4/ta2ZdnakzE8K/gTbvQSbsUFykrnhXgiWKei/"
           "aV3wwpQcFfbJjWe1bLF2TkwfgRDOskmX4qkx+vBcd9FPi9CHKL2upuBs4mUAxmQGK6OF5lcYSu9LsrTTlUQerVcaNTYHZN/"
           "wvIjN1lsyE5aDp9XUN9x+KeRNrwv6niIJ4noBBvhGEq4p3",
    .fingerprint = "SHA256:dzlWkSb7OqeiGGiN6wuSc6ici+NhkXdwDQG//O97T5c"};
constexpr TestKey ecdsa_key{.type = "ecdsa-sha2-nistp256",
                            .key = "AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBImRejS9ErAh1OOdC47ZSRieKYuog30Kmbj"
                                   "Ne2eU1a83lUtUlQ4zWKV3as/jvRuK0Y3ZoA59R25yQ3xwJpQyNMU=",
                            .fingerprint = "SHA256:jnKipMU5WnUEFe9Uw3SyzXTZ824ZukMN3TOBOmqNYC0"};
constexpr TestKey ed25519_key{.type = "ssh-ed25519",
                              .key = "AAAAC3NzaC1lZDI1NTE5AAAAINXtya8ykvjW/uP06S6LBhpJ4oA2XyaMoBsjOTzK04lA",
                              .fingerprint = "SHA256:9T6ok/ce9+od1GJHlM/pjoCfNa0fm/S7eYRLU1MX3MU"};

std::string blob_of(const TestKey& k) { return util::base64_decode(k.key).value_or(""); }
ssh::HostKey key_of(const TestKey& k) { return ssh::host_key(blob_of(k)).value_or(ssh::HostKey{}); }

// A blob that names `type` and holds nothing else: all a scan's bookkeeping looks at.
std::string fake_blob(std::string_view type) {
    std::string blob(4, '\0');
    blob[3] = static_cast<char>(type.size());
    return blob + std::string(type) + std::string("\0\0\0\1k", 5);
}

// A server with these host key types (as blob types), answering scanning handshakes as SSH's
// negotiation does: the first algorithm offered that it has. Records the offers.
struct FakeKeyServer {
    explicit FakeKeyServer(std::vector<std::string> types, std::optional<std::size_t> fail = std::nullopt)
        : has(std::move(types)), fail_at(fail) {}
    std::vector<std::string> has;
    std::optional<std::size_t> fail_at; // that handshake fails for another reason (the link)
    std::vector<std::string> offers;
    ssh::Shake shake() {
        return [this](std::string_view offer) -> std::expected<std::string, ssh::ShakeError> {
            offers.emplace_back(offer);
            if (fail_at && offers.size() == *fail_at + 1)
                return std::unexpected(ssh::ShakeError{.no_match = false, .message = "connection reset"});
            for (std::size_t b = 0; b <= offer.size();) {
                const std::size_t e = std::min(offer.find(',', b), offer.size());
                const std::string alg(offer.substr(b, e - b));
                const std::string type = alg.starts_with("rsa-sha2-") ? "ssh-rsa" : alg;
                if (std::ranges::find(has, type) != has.end()) return fake_blob(type);
                b = e + 1;
            }
            return std::unexpected(ssh::ShakeError{.no_match = true, .message = "Unable to exchange encryption keys"});
        };
    }
};

std::vector<std::string> types_of(const std::vector<ssh::HostKey>& keys) {
    std::vector<std::string> out;
    for (const auto& k : keys) out.push_back(k.type);
    return out;
}

} // namespace

TEST_CASE("ssh: a host key's type, fingerprint and known_hosts line, as ssh-keygen shows them") {
    for (const auto& k : {rsa_key, ecdsa_key, ed25519_key}) {
        const std::string blob = blob_of(k);
        CHECK(ssh::key_type(blob) == k.type);
        CHECK(ssh::fingerprint(blob) == k.fingerprint);
        REQUIRE(ssh::host_key(blob));
        const ssh::HostKey key = ssh::host_key(blob).value_or(ssh::HostKey{});
        CHECK(key.key == k.key);
        CHECK(ssh::known_host_line("box.example", 23, key) == std::format("[box.example]:23 {} {}", k.type, k.key));
        CHECK(ssh::known_host_line("plain.example", 22, key) == std::format("plain.example {} {}", k.type, k.key));
    }
    CHECK(ssh::key_type("").empty());
    CHECK(ssh::key_type(std::string("\0\0\0\x09ssh-", 8)).empty()); // longer than the blob
    CHECK(ssh::key_type(std::string("\0\0\0\x03" "a\nb", 7)).empty()); // not a name
    CHECK_FALSE(ssh::host_key("junk"));
    CHECK(util::base64_encode("ab") == "YWI=");
    CHECK(util::base64_decode("YWI") == "ab");
    CHECK_FALSE(util::base64_decode("Y W"));
}

TEST_CASE("ssh: a scanned key's line is one the login checks against (RSA as ssh-rsa, SHA-2 asked for)") {
    for (const auto& k : {rsa_key, ecdsa_key, ed25519_key}) {
        const std::string blob = blob_of(k);
        const std::string line = ssh::known_host_line("box.example", 23, key_of(k)) + "\n";
        CHECK(ssh::pinned_algorithms(line, "box.example", 23) ==
              (k.type == "ssh-rsa" ? std::string("rsa-sha2-512,rsa-sha2-256") : std::string(k.type)));
        CHECK(ssh::check_known_host(line, "box.example", 23, blob) == ssh::KnownHost::match);
        CHECK(ssh::check_known_host(line, "box.example", 2222, blob) == ssh::KnownHost::unknown);
        // The same type with other bytes: the key changed.
        std::string other = blob;
        other.back() = static_cast<char>(other.back() ^ 1);
        CHECK(ssh::check_known_host(line, "box.example", 23, other) == ssh::KnownHost::mismatch);
        const std::string plain = ssh::known_host_line("plain.example", 22, key_of(k));
        CHECK(ssh::check_known_host(plain, "plain.example", 22, blob) == ssh::KnownHost::match);
    }
    // Another type than the one pinned isn't known; a hashed name is.
    const std::string ed_line = ssh::known_host_line("box.example", 23, key_of(ed25519_key));
    CHECK(ssh::check_known_host(ed_line, "box.example", 23, blob_of(rsa_key)) == ssh::KnownHost::unknown);
    CHECK(ssh::check_known_host(std::format("|1|63e5q7mt43md8U/bjWpnzvn8MwI=|hYRuAw/Is4vG99BeZ3UIYjHhdfY= ssh-ed25519 {}\n",
                                            ed25519_key.key),
                                "box.example", 23, blob_of(ed25519_key)) == ssh::KnownHost::match);
}

TEST_CASE("ssh: a scan offers the key types not found yet and ends at the first the server can't agree on") {
    const std::string all = "ssh-ed25519,ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256";
    SUBCASE("ED25519 only (Main): 2 handshakes, one failed") {
        FakeKeyServer server({"ssh-ed25519"});
        auto keys = ssh::scan_host_keys(server.shake(), "main:39909");
        REQUIRE(keys);
        CHECK(types_of(*keys) == std::vector<std::string>{"ssh-ed25519"});
        CHECK(server.offers ==
              std::vector<std::string>{all, "ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256"});
    }
    SUBCASE("a Storage Box's three: 4 handshakes; RSA is one key for both SHA-2 algorithms") {
        FakeKeyServer server({"ssh-rsa", "ecdsa-sha2-nistp521", "ssh-ed25519"});
        auto keys = ssh::scan_host_keys(server.shake(), "box:23");
        REQUIRE(keys);
        CHECK(types_of(*keys) == std::vector<std::string>{"ssh-ed25519", "ecdsa-sha2-nistp521", "ssh-rsa"});
        CHECK(server.offers ==
              std::vector<std::string>{all, "ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521,rsa-sha2-512,rsa-sha2-256",
                                       "ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,rsa-sha2-512,rsa-sha2-256",
                                       "ecdsa-sha2-nistp256,ecdsa-sha2-nistp384"});
    }
    SUBCASE("every type: one handshake each, none failed") {
        FakeKeyServer server({"ssh-ed25519", "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384", "ecdsa-sha2-nistp521", "ssh-rsa"});
        auto keys = ssh::scan_host_keys(server.shake(), "all:22");
        REQUIRE(keys);
        CHECK(keys->size() == 5);
        CHECK(server.offers.size() == 5);
    }
    SUBCASE("nothing grab can check (DSA, SHA-1 RSA only): an error that says so") {
        FakeKeyServer server({"ssh-dss"});
        auto keys = ssh::scan_host_keys(server.shake(), "old:22");
        REQUIRE_FALSE(keys);
        CHECK(keys.error().find("old:22 has no host key grab can check") != std::string::npos);
        CHECK(server.offers.size() == 1);
    }
    SUBCASE("a handshake failing otherwise fails the scan: no key type is left out unsaid") {
        FakeKeyServer server({"ssh-ed25519", "ssh-rsa"}, 1);
        auto keys = ssh::scan_host_keys(server.shake(), "flaky:22");
        REQUIRE_FALSE(keys);
        CHECK(keys.error() == "can't read the host keys of flaky:22: connection reset");
    }
    SUBCASE("a key of a type not asked for ends it") {
        const ssh::Shake stubborn = [](std::string_view) -> std::expected<std::string, ssh::ShakeError> {
            return fake_blob("ssh-ed25519");
        };
        auto keys = ssh::scan_host_keys(stubborn, "odd:22");
        REQUIRE_FALSE(keys);
        CHECK(keys.error().find("showed a ssh-ed25519 key when grab asked for ecdsa") != std::string::npos);
    }
    SUBCASE("stopped: before the next handshake") {
        std::stop_source stop;
        FakeKeyServer server({"ssh-ed25519", "ssh-rsa"});
        auto inner = server.shake();
        const ssh::Shake stopping = [&](std::string_view offer) {
            stop.request_stop();
            return inner(offer);
        };
        auto keys = ssh::scan_host_keys(stopping, "s:22", stop.get_token());
        REQUIRE_FALSE(keys);
        CHECK(keys.error() == "cancelled");
        CHECK(server.offers.size() == 1);
    }
}

TEST_CASE("ssh: a scan of a port nobody listens on fails without waiting for its time") {
    const auto started = std::chrono::steady_clock::now();
    auto keys = ssh::scan_host_keys("127.0.0.1", 1, std::chrono::seconds(10));
    REQUIRE_FALSE(keys);
    CHECK(keys.error().starts_with("can't read the host keys of 127.0.0.1:1: can't reach 127.0.0.1:1"));
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(8));
}

TEST_CASE("ssh: rclone's times, and the connection flags grab's client takes from them (G189)") {
    using std::chrono::milliseconds;
    CHECK(ssh::parse_duration("30s") == milliseconds(30'000));
    CHECK(ssh::parse_duration("1m30s") == milliseconds(90'000));
    CHECK(ssh::parse_duration("1.5h") == milliseconds(5'400'000));
    CHECK(ssh::parse_duration("500ms") == milliseconds(500));
    CHECK(ssh::parse_duration("10") == milliseconds(10'000)); // a bare number: seconds
    CHECK(ssh::parse_duration("2d") == milliseconds(172'800'000));
    CHECK(ssh::parse_duration("1us") == milliseconds(1)); // under a millisecond, but some
    CHECK(ssh::parse_duration("0") == milliseconds(0));
    CHECK(ssh::parse_duration("off") == milliseconds(0));
    // Past 2^31-1 ms, Winsock's select and libssh2 would wrap it round: no limit instead.
    CHECK(ssh::parse_duration("24d") == milliseconds(2'073'600'000));
    CHECK(ssh::parse_duration("100y") == milliseconds(0));
    CHECK(ssh::parse_duration("4294968s") == milliseconds(0));
    CHECK(ssh::parse_duration("50d") == milliseconds(0));
    for (const char* bad : {"", "s", "1m30", "-5s", "1x", "1..5s", "5 s", "100000y"}) {
        CHECK_MESSAGE(!ssh::parse_duration(bad), bad);
    }

    ssh::Target t;
    const std::vector<std::string> flags{"--sftp-chunk-size", "255Ki", "--bind=10.0.0.2", "--contimeout", "15s",
                                         "--timeout=2m", "--sftp-disable-hashcheck", "--low-level-retries", "3"};
    auto unused = ssh::apply_flags(t, flags);
    REQUIRE(unused);
    CHECK(t.bind == "10.0.0.2");
    CHECK(t.connect_timeout == milliseconds(15'000));
    CHECK(t.timeout == milliseconds(120'000));
    CHECK(*unused == std::vector<std::string>{"--sftp-chunk-size", "255Ki", "--sftp-disable-hashcheck",
                                             "--low-level-retries", "3"});
    // Without them the defaults stay.
    ssh::Target plain;
    REQUIRE(ssh::apply_flags(plain, std::vector<std::string>{"--sftp-chunk-size", "255Ki"}));
    CHECK(plain == ssh::Target{});
    CHECK_FALSE(ssh::apply_flags(t, std::vector<std::string>{"--timeout", "soon"}));
    CHECK_FALSE(ssh::apply_flags(t, std::vector<std::string>{"--bind"}));
    CHECK_FALSE(ssh::apply_flags(t, std::vector<std::string>{"--contimeout", "--timeout", "5s"}));
}

TEST_CASE("ssh: --bind connects from that address, and one this machine can't use is fatal") {
    ssh::Target t;
    t.host = "127.0.0.1";
    t.port = 1; // nothing listens
    t.connect_timeout = std::chrono::seconds(5);
    auto kind_of = [&](const std::string& bind) {
        t.bind = bind;
        try {
            ssh::Connection c(t);
        } catch (const ssh::Error& e) {
            const std::string said = bind + ": " + e.what();
            MESSAGE(said);
            return std::pair{e.kind, std::string(e.what())};
        }
        return std::pair{budget::Failure::other, std::string("connected")};
    };
    const auto here = kind_of("127.0.0.1");
    CHECK(here.first == budget::Failure::other); // bound, then refused: the server's matter
    CHECK(here.second.starts_with("can't reach 127.0.0.1:1"));
    const auto foreign = kind_of("192.0.2.1"); // TEST-NET-1: never this machine's
    CHECK(foreign.first == budget::Failure::fatal);
    CHECK(foreign.second.find("--bind") != std::string::npos);
    const auto v6 = kind_of("::1");
    CHECK(v6.first == budget::Failure::fatal);
    CHECK(v6.second.find("no address of the same kind") != std::string::npos);
}

TEST_CASE("ssh: the offered algorithms leave out the weak ones") {
    for (std::string_view weak : {"sha1", "cbc", "arcfour", "md5", "3des", "blowfish", "group1-"}) {
        CHECK(ssh::kex_algorithms.find(weak) == std::string_view::npos);
        CHECK(ssh::cipher_algorithms.find(weak) == std::string_view::npos);
        CHECK(ssh::mac_algorithms.find(weak) == std::string_view::npos);
    }
}

TEST_CASE("ssh: a server's target from its grab.conf section and the secrets saved for its login") {
    server_conf::Connection c;
    c.host = "box.example";
    c.user = "u1";
    c.port = 23;
    c.auth = server_conf::Auth::password;
    const passwords::Binding login{.user = "u1", .host = "box.example", .port = 23};
    passwords::Store store;
    const std::filesystem::path grab_known = "C:/grab/known_hosts";
    const auto save = [&](std::string_view key, std::string value, passwords::Binding at) {
        store["Box"][std::string(key)] = passwords::Secret{.binding = std::move(at), .value = std::move(value)};
    };

    SUBCASE("a password: plain from the store, grab's known_hosts") {
        save(passwords::password, "fake-potato", login);
        auto t = ssh::target_for("Box", c, store, grab_known);
        REQUIRE(t);
        CHECK(t->host == "box.example");
        CHECK(t->port == 23);
        CHECK(t->user == "u1");
        CHECK(t->password == "fake-potato");
        CHECK_FALSE(t->agent);
        CHECK(t->known_hosts == grab_known);
    }
    SUBCASE("a password saved for another login is refused, never sent (G40)") {
        save(passwords::password, "fake-potato", passwords::Binding{.user = "u1", .host = "other.example", .port = 23});
        const auto t = ssh::target_for("Box", c, store, grab_known);
        REQUIRE_FALSE(t);
        CHECK(t.error().find("the saved password is for u1@other.example:23") != std::string::npos);
        CHECK(t.error().find("fake-potato") == std::string::npos);
        store.clear();
        CHECK(ssh::target_for("Box", c, store, grab_known).error_or("").find("no password is saved") != std::string::npos);
    }
    SUBCASE("ssh-agent") {
        c.auth = server_conf::Auth::agent;
        auto t = ssh::target_for("Box", c, store, grab_known);
        REQUIRE(t);
        CHECK(t->agent);
        CHECK(t->password.empty());
    }
    SUBCASE("a key file, its passphrase, and a password beside it that rclone tries too") {
        c.auth = server_conf::Auth::key;
        c.key_file = "C:/keys/id_ed25519";
        save(passwords::passphrase, "fake-phrase", login);
        save(passwords::password, "fake-pw", login);
        auto t = ssh::target_for("Box", c, store, grab_known);
        REQUIRE(t);
        CHECK(t->key_file == std::filesystem::path("C:/keys/id_ed25519"));
        CHECK(t->key_passphrase == "fake-phrase");
        CHECK(t->password == "fake-pw");
    }
    SUBCASE("what it can't use is said, not guessed") {
        c.auth = server_conf::Auth::stored_key;
        save(passwords::key_pem, "-----BEGIN fake-----", login);
        CHECK(ssh::target_for("Box", c, store, grab_known).error_or("").find("engine = rclone only") != std::string::npos);
        c.auth = server_conf::Auth::password;
        save(passwords::password, "fake", login);
        c.user.clear();
        CHECK_FALSE(ssh::target_for("Box", c, store, grab_known));
    }
}

TEST_CASE("ssh exec: stdout streams in pieces, stdin ends at once, stderr is kept up to its limit") {
    test::FakeCommand cmd;
    cmd.out = {"a\n", "bb\n", "ccc\n"};
    cmd.err = "warning\n";
    cmd.status = 1;
    cmd.start_waits = 2;
    std::vector<std::string> pieces;
    ssh::ExecOptions o;
    o.out = [&](std::string_view piece) {
        pieces.emplace_back(piece);
        return true;
    };
    o.err_limit = 4;
    auto r = ssh::run_command(cmd, "find .", o);
    REQUIRE(r);
    CHECK(cmd.command == "find .");
    CHECK(pieces == cmd.out);
    CHECK(r->out_bytes == 9);
    CHECK(r->err == "warn");
    CHECK(r->err_cut);
    CHECK(r->status == 1);
    CHECK(r->signal.empty());
    CHECK(cmd.log == std::vector<std::string>{"start", "eof", "close"});
    CHECK_FALSE(cmd.dropped);
}

TEST_CASE("ssh exec: a command reading its stdin ends, as EOF goes right after the exec request") {
    test::FakeCommand cmd; // cat, or the sftp-server of an internal-sftp account
    cmd.needs_eof = true;
    ssh::ExecOptions o;
    o.deadline = std::chrono::seconds(5); // a failure here instead of a hang
    auto r = ssh::run_command(cmd, "cat", o);
    REQUIRE(r);
    CHECK(r->status == 0);
    CHECK(r->out_bytes == 0);
    REQUIRE(cmd.log.size() >= 2);
    CHECK(cmd.log[0] == "start");
    CHECK(cmd.log[1] == "eof");
}

TEST_CASE("ssh exec: the end by a signal comes back without an exit status") {
    test::FakeCommand cmd;
    cmd.signal = "TERM";
    auto r = ssh::run_command(cmd, "sleep 60", {});
    REQUIRE(r);
    CHECK(r->status == -1);
    CHECK(r->signal == "TERM");
}

TEST_CASE("ssh exec: cancel sends TERM and drops the channel, looking at least once a second") {
    test::FakeCommand cmd;
    cmd.forever = true; // a silent sleep 60
    std::stop_source stop;
    ssh::ExecOptions o;
    o.stop = stop.get_token();
    std::jthread cancel([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        stop.request_stop();
    });
    auto started = std::chrono::steady_clock::now();
    auto r = ssh::run_command(cmd, "sleep 60", o);
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == ssh::ExecFailure::stopped);
    REQUIRE(cmd.log.size() >= 2);
    CHECK(cmd.log[cmd.log.size() - 2] == "TERM");
    CHECK(cmd.log.back() == "drop");
    CHECK(cmd.dropped);
    CHECK(cmd.longest_wait <= std::chrono::seconds(1)); // no wait longer than a second
    CHECK(cmd.longest_wait > std::chrono::milliseconds(500)); // (and the keepalive isn't due for 15 s)
}

TEST_CASE("ssh exec: endless output stops at the cap") {
    test::FakeCommand cmd;
    cmd.endless = true; // yes
    std::uint64_t got = 0;
    ssh::ExecOptions o;
    o.out = [&](std::string_view piece) {
        got += piece.size();
        return true;
    };
    SUBCASE("the byte cap") {
        o.out_limit = 100'000;
        auto r = ssh::run_command(cmd, "yes", o);
        REQUIRE_FALSE(r);
        CHECK(r.error().kind == ssh::ExecFailure::too_much);
        CHECK(got == 100'000); // every byte up to the cap, none past it
    }
    SUBCASE("the reader says stop (an entry cap)") {
        int pieces = 0;
        o.out = [&](std::string_view) { return ++pieces < 3; };
        auto r = ssh::run_command(cmd, "yes", o);
        REQUIRE_FALSE(r);
        CHECK(r.error().kind == ssh::ExecFailure::too_much);
        CHECK(pieces == 3);
    }
    CHECK(cmd.count("TERM") == 1);
    CHECK(cmd.log.back() == "drop");
}

TEST_CASE("ssh exec: past the deadline it's stopped, with what stderr said") {
    test::FakeCommand cmd;
    cmd.forever = true;
    cmd.err = "Welcome to the server\n"; // a banner, then nothing
    ssh::ExecOptions o;
    o.deadline = std::chrono::milliseconds(50);
    auto started = std::chrono::steady_clock::now();
    auto r = ssh::run_command(cmd, "env LC_ALL=C find -L . -maxdepth 0 -printf 'grab-find-ok\\n'", o);
    auto took = std::chrono::steady_clock::now() - started;
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == ssh::ExecFailure::deadline);
    CHECK(r.error().err == "Welcome to the server\n");
    CHECK(took >= std::chrono::milliseconds(50));
    CHECK(took < std::chrono::milliseconds(500));
    CHECK(cmd.count("TERM") == 1);
    CHECK(cmd.log.back() == "drop");
}

TEST_CASE("ssh exec: a refused exec request is its own kind and leaves the connection as it was") {
    test::FakeCommand cmd;
    cmd.refuse = true;
    auto r = ssh::run_command(cmd, "find .", {});
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == ssh::ExecFailure::refused);
    CHECK(cmd.log == std::vector<std::string>{"start", "close"}); // no EOF, TERM or drop
    CHECK_FALSE(cmd.dropped);
    CHECK(r.error().message == "the server refused to run the command");
}

TEST_CASE("ssh exec: a refused channel is refused too, with the server's reason") {
    test::FakeCommand cmd; // MaxSessions 1: the pooled session's SFTP channel is the one
    cmd.refuse_channel = "Channel open failure (connect failed)";
    auto r = ssh::run_command(cmd, "find .", {});
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == ssh::ExecFailure::refused);
    CHECK(r.error().message == "the server refused to run the command: Channel open failure (connect failed)");
    CHECK(cmd.log == std::vector<std::string>{"start", "close"});
    CHECK_FALSE(cmd.dropped);
}

TEST_CASE("ssh exec: keepalives keep a silent command alive; a link that answers none is dead") {
    // Times well above Windows' 15.6 ms sleep granularity (FakeCommand::wait), so that a late
    // tick under load doesn't change the outcome.
    test::FakeCommand cmd;
    ssh::ExecOptions o;
    o.keepalive = std::chrono::milliseconds(10);
    o.dead_after = std::chrono::milliseconds(200);
    SUBCASE("silent for longer than dead_after, answering each keepalive") {
        cmd.silent = std::chrono::milliseconds(400);
        cmd.out = {"done\n"};
        auto r = ssh::run_command(cmd, "sleep 1; echo done", o);
        REQUIRE(r);
        CHECK(r->status == 0);
        CHECK(r->out_bytes == 5);
        CHECK(cmd.count("keepalive") >= 5);
        CHECK_FALSE(cmd.dropped);
    }
    SUBCASE("no answers: an error, the channel dropped without TERM") {
        cmd.forever = true;
        cmd.answers = false;
        auto started = std::chrono::steady_clock::now();
        std::string why;
        try {
            auto r = ssh::run_command(cmd, "sleep 60", o);
            FAIL("no error; it ended " << (r ? "normally" : r.error().message));
        } catch (const ssh::Error& e) {
            why = e.what();
        }
        CHECK(why.starts_with("the server stopped answering"));
        CHECK(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(200));
        CHECK(cmd.count("keepalive") >= 3);
        CHECK(cmd.count("TERM") == 0);
        CHECK(cmd.log.back() == "drop dead");
    }
    SUBCASE("output keeps it alive without keepalives") {
        cmd.out = std::vector<std::string>(50, "line\n");
        auto r = ssh::run_command(cmd, "find .", o);
        REQUIRE(r);
        CHECK(cmd.count("keepalive") == 0);
    }
}

TEST_CASE("ssh exec: a session without a login refuses commands") {
    struct Plain : ssh::Session {
        sftp::Client& sftp() override { throw std::logic_error("no SFTP in this test"); }
    };
    Plain session;
    auto r = session.exec("find .", {});
    REQUIRE_FALSE(r);
    CHECK(r.error().kind == ssh::ExecFailure::refused);
}

namespace {

// Live tests run against a real server only when GRAB_TEST_SSH=user@host:port and
// GRAB_TEST_KNOWN_HOSTS name one (logging in through ssh-agent). CI sets neither.
std::optional<ssh::Target> live_target() {
    auto where = util::getenv_utf8("GRAB_TEST_SSH");
    auto known = util::getenv_utf8("GRAB_TEST_KNOWN_HOSTS");
    if (!where || !known) return std::nullopt;
    const std::string& w = *where;
    ssh::Target t;
    t.user = w.substr(0, w.find('@'));
    t.host = w.substr(w.find('@') + 1, w.rfind(':') - w.find('@') - 1);
    t.port = std::stoi(w.substr(w.rfind(':') + 1));
    t.known_hosts = util::path_from_utf8(*known);
    t.agent = true;
    return t;
}

// Whether `target` is turned away as fatal with a known_hosts holding only `line` for it.
bool refused_with(const ssh::Target& target, const std::string& line) {
    ssh::Target t = target;
    t.known_hosts = std::filesystem::temp_directory_path() / "grab-test-known_hosts";
    std::ofstream(t.known_hosts) << std::format("[{}]:{} {}\n", t.host, t.port, line);
    bool fatal = false;
    try {
        ssh::Connection c(t);
    } catch (const ssh::Error& e) {
        fatal = e.kind == budget::Failure::fatal;
        MESSAGE(std::string(e.what()));
    }
    std::filesystem::remove(t.known_hosts);
    return fatal;
}

} // namespace

TEST_CASE("ssh: live login, SFTP and a command (GRAB_TEST_SSH)") {
    auto t = live_target();
    if (!t) {
        MESSAGE("skipped: GRAB_TEST_SSH / GRAB_TEST_KNOWN_HOSTS not set");
        return;
    }
    ssh::Connection c(*t);
    MESSAGE("algorithms: " << c.algorithms() << ", login " << c.login_time().count() << " ms");
    auto out = c.run("echo out; echo err >&2; exit 3");
    CHECK(out.out == "out\n");
    CHECK(out.err == "err\n");
    CHECK(out.status == 3);
    CHECK_FALSE(out.cut);
    auto big = c.run("head -c 300000 /dev/zero", 1000);
    CHECK(big.out.size() == 1000);
    CHECK(big.cut);
    CHECK(big.status == 0);

    auto channel = c.open_sftp();
    sftp::Client s(*channel);
    s.start();
    auto home = sftp::realpath(s, ".");
    REQUIRE(home);
    auto entries = sftp::list(s, *home);
    REQUIRE(entries);
    CHECK_FALSE(entries->empty());
    CHECK(c.received() > 0);
    // A keepalive on the idle SFTP session (Play's kept connections): no answer comes, SFTP goes on.
    CHECK(c.keepalive());
    const auto again = sftp::realpath(s, ".");
    REQUIRE(again);
    CHECK(*again == *home);
    CHECK_FALSE(c.dropped());
}

// Phase 4's check of exec on a real server, on one login: EOF, keepalives, cancel and the cap.
// Nothing is left running there (TERM ends sleep and yes).
TEST_CASE("ssh: live exec, EOF, keepalives, cancel and the cap on one login (GRAB_TEST_SSH)") {
    auto t = live_target();
    if (!t) {
        MESSAGE("skipped: GRAB_TEST_SSH / GRAB_TEST_KNOWN_HOSTS not set");
        return;
    }
    using Clock = std::chrono::steady_clock;
    auto seconds = [](Clock::duration d) { return std::chrono::duration<double>(d).count(); };
    ssh::Connection c(*t);

    // stdin ends at once, so cat has nothing to copy and ends.
    auto started = Clock::now();
    auto cat = c.exec("cat");
    auto took = Clock::now() - started;
    REQUIRE_MESSAGE(cat.has_value(), (cat ? "" : cat.error().message));
    CHECK(cat->status == 0);
    CHECK(cat->out_bytes == 0);
    CHECK(took < std::chrono::seconds(3));
    MESSAGE("cat: " << seconds(took) << " s");

    // Silent for longer than dead_after: only the keepalives' answers keep it alive. It ends
    // normally and the connection works on.
    ssh::ExecOptions quiet;
    quiet.keepalive = std::chrono::seconds(3);
    quiet.dead_after = std::chrono::seconds(7);
    auto before = c.received();
    started = Clock::now();
    auto slept = c.exec("sleep 10", quiet);
    took = Clock::now() - started;
    REQUIRE_MESSAGE(slept.has_value(), (slept ? "" : slept.error().message));
    CHECK(slept->status == 0);
    CHECK(took >= std::chrono::seconds(10));
    CHECK_FALSE(c.dropped());
    MESSAGE("sleep 10: " << seconds(took) << " s, " << c.received() - before << " bytes received");
    CHECK(c.run("echo ok").out == "ok\n");

    // EOF at once, the exit status 3 s later: a keepalive's answer in between doesn't end the
    // wait for the server's close, which comes after the status.
    ssh::ExecOptions late;
    late.keepalive = std::chrono::seconds(1);
    started = Clock::now();
    auto closed = c.exec("exec >&- 2>&-; sleep 3; exit 3", late);
    took = Clock::now() - started;
    REQUIRE_MESSAGE(closed.has_value(), (closed ? "" : closed.error().message));
    CHECK(closed->status == 3);
    CHECK(took >= std::chrono::seconds(3));
    MESSAGE("output closed, exit 3 after 3 s: " << seconds(took) << " s");

    // A silent sleep 60 stops on cancel: TERM, then the channel is dropped.
    std::stop_source stop;
    ssh::ExecOptions cancellable;
    cancellable.stop = stop.get_token();
    std::jthread cancel([&] {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        stop.request_stop();
    });
    started = Clock::now();
    auto slow = c.exec("sleep 60", cancellable);
    took = Clock::now() - started;
    REQUIRE_FALSE(slow.has_value());
    CHECK(slow.error().kind == ssh::ExecFailure::stopped);
    CHECK(took < std::chrono::seconds(3));
    CHECK(c.dropped()); // a pooled session wouldn't be reused; this test goes on with its link
    MESSAGE("sleep 60, cancelled after 1 s: " << seconds(took) << " s");

    // Endless output stops at the cap.
    ssh::ExecOptions capped;
    capped.out_limit = std::uint64_t{1024} * 1024;
    std::uint64_t got = 0;
    capped.out = [&](std::string_view piece) {
        got += piece.size();
        return true;
    };
    started = Clock::now();
    auto yes = c.exec("yes", capped);
    took = Clock::now() - started;
    REQUIRE_FALSE(yes.has_value());
    CHECK(yes.error().kind == ssh::ExecFailure::too_much);
    CHECK(got == 1024 * 1024);
    CHECK(took < std::chrono::seconds(5));
    MESSAGE("yes, cut at 1 MiB: " << seconds(took) << " s");
}

// What a command leaves behind on its connection, on one login: a refused channel (the server's
// session limit, MaxSessions 10 on OpenSSH) leaves the link and its SFTP channels as they were, and
// SFTP waits after a command still last the whole timeout (libssh2's keepalives are off again).
TEST_CASE("ssh: live, a connection after commands: refused channel, SFTP waits (GRAB_TEST_SSH)") {
    auto t = live_target();
    if (!t) {
        MESSAGE("skipped: GRAB_TEST_SSH / GRAB_TEST_KNOWN_HOSTS not set");
        return;
    }
    using Clock = std::chrono::steady_clock;
    auto seconds = [](Clock::duration d) { return std::chrono::duration<double>(d).count(); };
    t->timeout = std::chrono::seconds(5);
    ssh::Connection c(*t);
    auto ran = c.exec("true");
    REQUIRE_MESSAGE(ran.has_value(), (ran ? "" : ran.error().message));

    // SFTP channels until the server refuses one.
    std::vector<std::unique_ptr<sftp::Channel>> channels;
    bool full = false;
    while (!full && channels.size() < 32) {
        try {
            channels.push_back(c.open_sftp());
        } catch (const ssh::Error&) {
            full = true;
        }
    }
    REQUIRE(channels.size() >= 2);
    if (full) {
        auto refused = c.exec("true");
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().kind == ssh::ExecFailure::refused);
        CHECK_FALSE(c.dropped());
        MESSAGE(channels.size() << " SFTP channels, then: " << refused.error().message);
    } else {
        MESSAGE("the server took " << channels.size() << " channels: no refused channel to try");
    }
    sftp::Client s(*channels[0]);
    s.start();
    CHECK(sftp::realpath(s, "."));

    // A channel the server says nothing on (sftp-server waits for its INIT): the read gives up
    // after the session's timeout, not after the 1-2 s of libssh2's keepalive interval.
    std::array<char, 16> buffer{};
    auto started = Clock::now();
    CHECK_THROWS_AS((void)channels[1]->read(buffer), sftp::Broken);
    auto took = Clock::now() - started;
    CHECK(took >= t->timeout - std::chrono::seconds(1));
    MESSAGE("a silent SFTP channel gave up after " << seconds(took) << " s");
}

TEST_CASE("ssh: live pool, 4 threads twice over, 4 logins (GRAB_TEST_SSH)") {
    auto t = live_target();
    if (!t) {
        MESSAGE("skipped: GRAB_TEST_SSH / GRAB_TEST_KNOWN_HOSTS not set");
        return;
    }
    std::atomic<int> logins = 0;
    ssh::Pool::Options o;
    o.limit = 4;
    ssh::Pool pool([&] { ++logins; return ssh::open_session(*t); }, o);
    for (int round = 0; round < 2; ++round) {
        std::atomic<int> ok = 0;
        {
            std::vector<std::jthread> threads;
            for (int i = 0; i < 4; ++i) {
                threads.emplace_back([&] {
                    auto lease = pool.acquire();
                    if (lease && sftp::realpath((*lease)->sftp(), ".")) ++ok;
                    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // all four held at once
                });
            }
        }
        CHECK(ok == 4);
    }
    CHECK(logins == 4);
    CHECK(pool.idle() == 4);
}

// From a server's rclone.conf section to a login through the pool, as the engine will do it:
// GRAB_TEST_RCLONE_CONF names an rclone.conf (no passwords needed for an agent server) and
// GRAB_TEST_REMOTE its section, read as the import reads it (live_import.hpp, nothing written);
// GRAB_TEST_KNOWN_HOSTS, when set, the known_hosts it is checked against.
TEST_CASE("ssh: live, a server from its rclone.conf section through the pool (GRAB_TEST_REMOTE)") {
    auto conf = util::getenv_utf8("GRAB_TEST_RCLONE_CONF");
    auto name = util::getenv_utf8("GRAB_TEST_REMOTE");
    if (!conf || !name) {
        MESSAGE("skipped: GRAB_TEST_RCLONE_CONF / GRAB_TEST_REMOTE not set");
        return;
    }
    const auto known = util::getenv_utf8("GRAB_TEST_KNOWN_HOSTS");
    const auto known_hosts = known ? util::path_from_utf8(*known) : grab_known_hosts_path();
    auto server = test::live_server(util::path_from_utf8(*conf), *name, known_hosts);
    REQUIRE_MESSAGE(server, server.error_or(""));
    auto target = ssh::target_for(*name, server->server.connection, server->store, known_hosts);
    REQUIRE_MESSAGE(target, target.error_or(""));
    ssh::Pool::Options o;
    o.limit = 2;
    ssh::Pool pool([&] { return ssh::open_session(*target); }, o);
    auto lease = pool.acquire();
    REQUIRE(lease);
    auto home = sftp::realpath((*lease)->sftp(), ".");
    REQUIRE(home);
    MESSAGE("home: " << *home);
}

TEST_CASE("ssh: live, a changed or missing host key ends it before the login (GRAB_TEST_SSH)") {
    auto t = live_target();
    if (!t) {
        MESSAGE("skipped: GRAB_TEST_SSH / GRAB_TEST_KNOWN_HOSTS not set");
        return;
    }
    // Right type, other bytes: the key changed.
    CHECK(refused_with(*t, "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIEJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJCQkJC"));
    // Nothing for this host at all.
    ssh::Target none = *t;
    none.known_hosts = std::filesystem::temp_directory_path() / "grab-test-no-known_hosts";
    CHECK_THROWS_AS(ssh::Connection{none}, ssh::Error);
}

// A real scan of the live server's host keys, compared with GRAB_TEST_KNOWN_HOSTS; nothing is
// written. Only with GRAB_TEST_SCAN set as well: where the server lacks a key type the scan ends
// with a failed negotiation, which fail2ban's aggressive mode counts, so the other live runs don't
// scan.
TEST_CASE("ssh: live, a host-key scan finds the keys pinned for the server (GRAB_TEST_SCAN)") {
    auto t = live_target();
    if (!t || !util::getenv_utf8("GRAB_TEST_SCAN")) {
        MESSAGE("skipped: GRAB_TEST_SSH / GRAB_TEST_KNOWN_HOSTS / GRAB_TEST_SCAN not set");
        return;
    }
    const auto started = std::chrono::steady_clock::now();
    auto keys = ssh::scan_host_keys(t->host, t->port, std::chrono::seconds(10));
    REQUIRE_MESSAGE(keys, keys.error_or(""));
    MESSAGE("scan: " << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << " s");
    const std::string pinned = util::read_file(t->known_hosts).value_or("");
    CHECK_FALSE(keys->empty());
    for (const auto& k : *keys) {
        MESSAGE(k.type << " " << k.fingerprint);
        CHECK(ssh::check_known_host(pinned, t->host, t->port, util::base64_decode(k.key).value_or("")) ==
              ssh::KnownHost::match);
        CHECK(pinned.find(ssh::known_host_line(t->host, t->port, k)) != std::string::npos);
    }
}
