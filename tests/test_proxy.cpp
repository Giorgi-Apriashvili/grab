// grab's own client through a proxy (PROXIES.md, chunk 0): the forms a proxy is written in, and
// SOCKS5 and HTTP CONNECT handshakes against fake proxies on 127.0.0.1 (fake_proxy.hpp), which
// relay to a OneShotServer or send a server's banner themselves. A server named x.invalid can
// only be reached through the proxy: no one looks that name up.

#include "conn_budget.hpp"
#include "proxy.hpp"
#include "ssh.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <optional>
#include <string>

#ifdef _WIN32
#include "fake_proxy.hpp"
#include "one_shot_server.hpp"

#include <winsock2.h> // IWYU pragma: keep (the socket API)

#include <atomic>
#include <filesystem>
#include <fstream>
#include <format>
#include <string_view>
#include <thread>
#include <vector>
#endif

using namespace grab;
using proxy::Kind;

TEST_CASE("proxy: the forms grab.conf takes, read back from the text grab writes") {
    struct Form {
        std::string_view written;
        Kind kind;
        std::string_view host;
        int port;
        std::string_view user;
        std::string_view text;
    };
    for (const Form& f : {
             Form{.written = "socks5://127.0.0.1:9050", .kind = Kind::socks5, .host = "127.0.0.1", .port = 9050,
                  .user = "", .text = "socks5://127.0.0.1:9050"},
             Form{.written = "socks5h://alice@proxy.example:1080", .kind = Kind::socks5, .host = "proxy.example",
                  .port = 1080, .user = "alice",
                  .text = "socks5://alice@proxy.example:1080"},
             Form{.written = "  SOCKS5H://Tor-Box_1:9150/ ", .kind = Kind::socks5, .host = "Tor-Box_1", .port = 9150,
                  .user = "", .text = "socks5://Tor-Box_1:9150"},
             Form{.written = "http://proxy.example:3128", .kind = Kind::http, .host = "proxy.example", .port = 3128,
                  .user = "", .text = "http://proxy.example:3128"},
             Form{.written = "HTTP://bob@10.0.0.1:8080/", .kind = Kind::http, .host = "10.0.0.1", .port = 8080,
                  .user = "bob", .text = "http://bob@10.0.0.1:8080"},
             Form{.written = "socks5://[::1]:1080", .kind = Kind::socks5, .host = "::1", .port = 1080, .user = "",
                  .text = "socks5://[::1]:1080"},
             Form{.written = "http://[2001:db8::7]:3128", .kind = Kind::http, .host = "2001:db8::7", .port = 3128,
                  .user = "", .text = "http://[2001:db8::7]:3128"},
             Form{.written = "socks5://al%40ice%2Fx@h:1", .kind = Kind::socks5, .host = "h", .port = 1,
                  .user = "al@ice/x", .text = "socks5://al%40ice%2Fx@h:1"},
             Form{.written = "http://d%C3%A9j%C3%A0@h:65535", .kind = Kind::http, .host = "h", .port = 65535,
                  .user = "d\xC3\xA9j\xC3\xA0",
                  .text = "http://d%C3%A9j%C3%A0@h:65535"},
         }) {
        CAPTURE(f.written);
        const auto p = proxy::parse(f.written);
        REQUIRE_MESSAGE(p, p.error_or(""));
        CHECK(p->kind == f.kind);
        CHECK(p->host == f.host);
        CHECK(p->port == f.port);
        CHECK(p->user == f.user);
        CHECK(p->password.empty());
        CHECK(proxy::text(*p) == f.text);
        CHECK(proxy::parse(proxy::text(*p)) == *p); // what grab writes it reads back
    }
    // The text never holds a password, whatever the Proxy has.
    const proxy::Proxy with{.kind = Kind::http, .host = "h", .port = 3128, .user = "alice", .password = "fake-secret"};
    CHECK(proxy::text(with) == "http://alice@h:3128");
    CHECK(proxy::text(proxy::Proxy{}).empty());
    CHECK(proxy::address("::1", 1080) == "[::1]:1080");
    CHECK(proxy::kind_name(Kind::socks5) == "SOCKS5");
    CHECK(proxy::kind_name(Kind::http) == "HTTP");
}

TEST_CASE("proxy: the forms refused, each with its reason, never quoting a password") {
    struct Refused {
        std::string_view written, reason;
    };
    for (const Refused& r : {
             Refused{.written = "socks4://h:1080", .reason = "grab can't use SOCKS4 proxies yet: use socks5://"},
             Refused{.written = "socks4a://h:1080", .reason = "grab can't use SOCKS4 proxies yet: use socks5://"},
             Refused{.written = "https://h:443", .reason = "grab can't use HTTPS proxies yet: use http://"},
             Refused{.written = "socks://h:1080", .reason = "say which SOCKS: grab can use socks5://"},
             Refused{.written = "ftp://h:21", .reason = "grab can use socks5:// and http:// proxies, not ftp://"},
             Refused{.written = "h:1080", .reason = "write the proxy as socks5://host:port or http://host:port"},
             Refused{.written = "", .reason = "write the proxy as socks5://host:port or http://host:port"},
             Refused{.written = "socks5://h", .reason = "give the proxy's port (socks5://host:1080)"},
             Refused{.written = "http://h/", .reason = "give the proxy's port (http://host:3128)"},
             Refused{.written = "socks5://h:", .reason = "the proxy's port must be from 1 to 65535"},
             Refused{.written = "socks5://h:0", .reason = "the proxy's port must be from 1 to 65535"},
             Refused{.written = "socks5://h:65536", .reason = "the proxy's port must be from 1 to 65535"},
             Refused{.written = "socks5://h:10x", .reason = "the proxy's port must be from 1 to 65535"},
             Refused{.written = "socks5://::1:1080",
                     .reason = "write the proxy's IPv6 address in brackets: [::1]:1080"},
             Refused{.written = "socks5://[::1:1080", .reason = "a ']' is missing after the proxy's IPv6 address"},
             Refused{.written = "socks5://[proxy]:1080",
                     .reason = "the proxy's address in brackets isn't an IPv6 address"},
             Refused{.written = "socks5://h:1/x",
                     .reason = "a proxy's address has no path: write it as socks5://host:1080"},
             Refused{.written = "socks5://@h:1", .reason = "give the proxy's user name before @, or leave the @ out"},
             Refused{.written = "socks5://a%zz@h:1", .reason = "the proxy's user name has a stray %: write % as %25"},
             Refused{.written = "socks5://a%0Ab@h:1",
                     .reason = "the proxy's user name can't hold spaces or control characters"},
             Refused{.written = "http://a%3Ab@h:1",
                     .reason = "the user name for an HTTP proxy can't hold ':' (Basic login)"},
             Refused{.written = "socks5://pro xy:1",
                     .reason = "use a host name (letters, digits, - and .) or an IP address for the proxy"},
             Refused{.written = "socks5://-h:1",
                     .reason = "use a host name (letters, digits, - and .) or an IP address for the proxy"},
         }) {
        CAPTURE(r.written);
        const auto p = proxy::parse(r.written);
        REQUIRE_FALSE(p);
        CHECK(p.error() == r.reason);
    }
    // Something before :// that isn't a scheme may be a login: never repeated.
    for (const std::string_view written : {"alice:fake-secret@socks5://h:1", "fake-secret.longer://h:1", "://h:1"}) {
        CAPTURE(written);
        const auto p = proxy::parse(written);
        REQUIRE_FALSE(p);
        CHECK(p.error() == "write the proxy as socks5://host:port or http://host:port");
        CHECK(p.error().find("fake-") == std::string::npos);
    }
    CHECK(proxy::parse("svn+ssh://h:1").error_or("") == "grab can use socks5:// and http:// proxies, not svn+ssh://");
    // A password in grab.conf's line: the line is refused, the password not repeated.
    for (const std::string_view written : {"socks5://alice:fake-secret@h:1080", "http://alice:fake-secret@h:3128",
                                           "socks5://alice:@h:1080"}) {
        const auto p = proxy::parse(written, "Box");
        REQUIRE_FALSE(p);
        CHECK(p.error() == "keep the proxy's password in grab's store, not grab.conf: take it out of the line, then Edit "
                           "Box and type it there");
    }
    const std::string user(256, 'u');
    CHECK(proxy::parse("socks5://" + user + "@h:1").error_or("") ==
          "the proxy's user name is longer than SOCKS5 allows (255 bytes)");
}

TEST_CASE("proxy: rclone's socks_proxy and http_proxy, read as rclone 1.75.1 read them") {
    using P = proxy::Proxy;
    // socks_proxy: split at the first '@', then the first ':'; nothing decoded.
    CHECK(proxy::parse_rclone_socks("alice:fake-pw@127.0.0.1:9050") ==
          P{.kind = Kind::socks5, .host = "127.0.0.1", .port = 9050, .user = "alice", .password = "fake-pw"});
    CHECK(proxy::parse_rclone_socks("alice@h:1080") ==
          P{.kind = Kind::socks5, .host = "h", .port = 1080, .user = "alice", .password = ""});
    CHECK(proxy::parse_rclone_socks("h:1080") == P{.kind = Kind::socks5, .host = "h", .port = 1080, .user = "", .password = ""});
    CHECK(proxy::parse_rclone_socks("[::1]:1080") ==
          P{.kind = Kind::socks5, .host = "::1", .port = 1080, .user = "", .password = ""});
    CHECK(proxy::parse_rclone_socks("a:p:w%41@h:1") ==
          P{.kind = Kind::socks5, .host = "h", .port = 1, .user = "a", .password = "p:w%41"});
    // A password with '@' broke rclone too (the rest became the host): refused, not guessed.
    const auto at = proxy::parse_rclone_socks("a:fake@pw@h:1");
    REQUIRE_FALSE(at);
    CHECK(at.error().find("fake") == std::string::npos);
    CHECK_FALSE(proxy::parse_rclone_socks("h"));
    CHECK_FALSE(proxy::parse_rclone_socks(":pw@h:1"));
    CHECK_FALSE(proxy::parse_rclone_socks("socks5://h:1080"));

    // http_proxy: a URL, its user and password percent-decoded; port 80 when it has none.
    CHECK(proxy::parse_rclone_http("http://alice:p%40ss%3Aw@proxy.example:3128") ==
          P{.kind = Kind::http, .host = "proxy.example", .port = 3128, .user = "alice", .password = "p@ss:w"});
    CHECK(proxy::parse_rclone_http("http://proxy.example") ==
          P{.kind = Kind::http, .host = "proxy.example", .port = 80, .user = "", .password = ""});
    CHECK(proxy::parse_rclone_http("HTTP://[::1]:8080/") ==
          P{.kind = Kind::http, .host = "::1", .port = 8080, .user = "", .password = ""});
    CHECK(proxy::parse_rclone_http("https://h:443").error_or("") == "grab can't use HTTPS proxies yet: use http://");
    CHECK(proxy::parse_rclone_http("socks5://h:1080").error_or("") == "an HTTP proxy's address starts with http://");
    CHECK_FALSE(proxy::parse_rclone_http("http://a:%zz@h:1"));
}

TEST_CASE("proxy: the copy for older grabs in rclone's form reads back; the said form holds no password") {
    using P = proxy::Proxy;
    for (const P& p : {P{.kind = Kind::socks5, .host = "127.0.0.1", .port = 9050, .user = "alice", .password = "fake-pw"},
                       P{.kind = Kind::socks5, .host = "h", .port = 1080, .user = "alice", .password = ""},
                       P{.kind = Kind::socks5, .host = "::1", .port = 1080, .user = "", .password = ""},
                       P{.kind = Kind::http, .host = "proxy.example", .port = 3128, .user = "al@ice", .password = "p@ss:w/%"},
                       P{.kind = Kind::http, .host = "2001:db8::7", .port = 8080, .user = "", .password = ""},
                       P{.kind = Kind::http, .host = "h", .port = 3128, .user = "bob", .password = ""}}) {
        const auto copy = proxy::rclone_text(p);
        CAPTURE(proxy::text(p));
        const auto back = p.kind == Kind::socks5 ? proxy::parse_rclone_socks(copy) : proxy::parse_rclone_http(copy);
        REQUIRE(back);
        CHECK(*back == p);
    }
    CHECK(proxy::rclone_text(P{.kind = Kind::socks5, .host = "h", .port = 1, .user = "a", .password = "fake-pw"}) ==
          "a:fake-pw@h:1");
    CHECK(proxy::rclone_text(P{.kind = Kind::http, .host = "h", .port = 1, .user = "a", .password = "f@ke"}) ==
          "http://a:f%40ke@h:1");
    CHECK(proxy::rclone_text(P{}).empty());
    const P alice{.kind = Kind::socks5, .host = "127.0.0.1", .port = 9050, .user = "alice", .password = "fake-pw"};
    CHECK(proxy::said(alice) == "the SOCKS5 proxy alice@127.0.0.1:9050");
    CHECK(proxy::said(P{.kind = Kind::http, .host = "::1", .port = 3128, .user = {}, .password = "fake-pw"}) ==
          "the HTTP proxy [::1]:3128");
    CHECK(proxy::said(P{}).empty());
}

TEST_CASE("proxy: what a proxy can't be asked for is refused before anything is sent") {
    const proxy::Proxy socks{.kind = Kind::socks5, .host = "127.0.0.1", .port = 1080, .user = "", .password = ""};
    proxy::Proxy http = socks;
    http.kind = Kind::http;
    CHECK_FALSE(proxy::check_request(socks, "box.example", 23));
    CHECK_FALSE(proxy::check_request(socks, "2001:db8::7", 22));
    CHECK_FALSE(proxy::check_request(proxy::Proxy{}, "anything at all", 0)); // directly: nothing to check
    CHECK(proxy::check_request(socks, std::string(256, 'a'), 22) == "the server's name is longer than a proxy can take (255 bytes)");
    CHECK_FALSE(proxy::check_request(socks, std::string(255, 'a'), 22));
    CHECK(proxy::check_request(http, "x.invalid\r\nHost: evil", 22) == "the server's name can't hold spaces or control characters");
    CHECK(proxy::check_request(socks, "b\xC3\xBC" "cher.example", 22) ==
          "write the server's name b\xC3\xBC" "cher.example in its xn-- form: a proxy takes only ASCII names");
    CHECK(proxy::check_request(socks, "fe80::1%eth0", 22) == "host fe80::1%eth0: an address with a zone can't go through a proxy");
    proxy::Proxy user = socks;
    user.user = "al ice";
    CHECK(proxy::check_request(user, "h", 22) == "the proxy's user name can't hold spaces or control characters");
    user.user = std::string(256, 'u');
    CHECK(proxy::check_request(user, "h", 22) == "the proxy's user name is longer than SOCKS5 allows (255 bytes)");
    user.user = "alice";
    user.password = std::string(256, 'p');
    CHECK(proxy::check_request(user, "h", 22) == "the proxy's password is longer than SOCKS5 allows (255 bytes)");
    user.password.clear(); // a user without a password: Tor's way of keeping streams apart
    CHECK_FALSE(proxy::check_request(user, "h", 22));
    user.kind = Kind::http;
    user.user = "al:ice";
    CHECK(proxy::check_request(user, "h", 22) == "the user name for an HTTP proxy can't hold ':' (Basic login)");
}

TEST_CASE("proxy: times as the messages say them") {
    using std::chrono::milliseconds;
    CHECK(proxy::say_time(milliseconds(120'000)) == "2 min");
    CHECK(proxy::say_time(milliseconds(30'000)) == "30 s");
    CHECK(proxy::say_time(milliseconds(90'000)) == "90 s");
    CHECK(proxy::say_time(milliseconds(1'500)) == "1.5 s");
    CHECK(proxy::say_time(milliseconds(500)) == "500 ms");
}

#ifdef _WIN32
namespace {

constexpr std::string_view ed25519_pin = "AAAAC3NzaC1lZDI1NTE5AAAAINXtya8ykvjW/uP06S6LBhpJ4oA2XyaMoBsjOTzK04lA";

// A server's first words that no client of grab's can agree with (SHA-1 group 1 only): a login
// that gets them ends "SSH handshake with ... failed" (fatal), which proves every byte of them
// reached libssh2 through the proxy.
std::string old_banner() {
    return "SSH-2.0-FakeServer\r\n" + test::kexinit_packet("diffie-hellman-group1-sha1", "ssh-ed25519", "aes128-ctr",
                                                           "hmac-sha2-256");
}

struct Ended {
    budget::Failure kind = budget::Failure::other;
    std::optional<budget::Failure> proxied;
    std::string what;
    std::chrono::milliseconds took{0};
};

// A login to host:port through `via`, a key pinned for it in a known_hosts of its own.
Ended log_in(const proxy::Proxy& via, const std::string& host = "x.invalid", int port = 2222,
             std::chrono::milliseconds connect_timeout = std::chrono::seconds(10), const std::string& bind = {},
             std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    const auto known_hosts = std::filesystem::temp_directory_path() / "grab-test-proxy-known_hosts";
    std::ofstream(known_hosts, std::ios::binary | std::ios::trunc)
        << std::format("{} ssh-ed25519 {}\n", port == 22 ? host : std::format("[{}]:{}", host, port), ed25519_pin);
    ssh::Target t;
    t.host = host;
    t.port = port;
    t.user = "u";
    t.agent = true;
    t.known_hosts = known_hosts;
    t.connect_timeout = connect_timeout;
    t.timeout = timeout;
    t.bind = bind;
    t.proxy = via;
    Ended out;
    const auto started = std::chrono::steady_clock::now();
    try {
        ssh::Connection c(t);
        out.what = "logged in";
    } catch (const ssh::Error& e) {
        out = Ended{.kind = e.kind, .proxied = e.proxied, .what = e.what(), .took = {}};
    }
    out.took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    std::filesystem::remove(known_hosts);
    return out;
}

// The server's banner and KEXINIT reached libssh2 whole.
bool reached_server(const Ended& e, std::string_view host = "x.invalid") {
    const bool reached = e.kind == budget::Failure::fatal &&
                         e.what.starts_with(std::format("SSH handshake with {} failed", host)) && !e.proxied;
    if (!reached) MESSAGE("it ended: " << e.what);
    return reached;
}

// A port on 127.0.0.1 nothing listens on now.
int dead_port() {
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int length = sizeof(at);
    int port = 0;
    if (::bind(s, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) == 0 &&
        ::getsockname(s, reinterpret_cast<sockaddr*>(&at), &length) == 0)
        port = ntohs(at.sin_port);
    closesocket(s);
    return port;
}

test::ProxyScript serving(Kind kind) {
    test::ProxyScript s;
    s.kind = kind;
    s.serve = old_banner();
    return s;
}

} // namespace

TEST_CASE("proxy: SOCKS5 without a login asks for the server by its name and hands libssh2 the tunnel") {
    test::OneShotServer server(old_banner().substr(20)); // its own banner, then that KEXINIT
    test::ProxyScript script;
    script.relay_port = server.port;
    test::FakeProxy fake(script);
    const auto ended = log_in(fake.proxy());
    CHECK(reached_server(ended));
    CHECK(server.accepted());
    const auto asked = fake.requests();
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].greeting == std::string("\x05\x01\x00", 3));
    CHECK(asked[0].login.empty());
    // ATYP 03 and the name as written: x.invalid was never looked up here (it can't be).
    CHECK(asked[0].connect == std::string("\x05\x01\x00\x03\x09x.invalid\x08\xae", 16));
}

TEST_CASE("proxy: SOCKS5 with a user name and password (RFC 1929); one turned down is fatal and never quoted") {
    test::ProxyScript script = serving(Kind::socks5);
    script.user = "alice";
    script.password = "fake-secret";
    test::FakeProxy fake(script);
    SUBCASE("accepted") {
        CHECK(reached_server(log_in(fake.proxy("alice", "fake-secret"))));
        const auto asked = fake.requests();
        REQUIRE(asked.size() == 1);
        CHECK(asked[0].greeting == std::string("\x05\x02\x00\x02", 4));
        CHECK(asked[0].login == std::string("\x01\x05" "alice\x0b" "fake-secret", 19));
    }
    SUBCASE("turned down: fatal, the server never asked for, the password in no message") {
        const auto ended = log_in(fake.proxy("alice", "fake-wrong"));
        CHECK(ended.kind == budget::Failure::fatal);
        CHECK(ended.what == "the proxy turned down the user name and password");
        CHECK(ended.what.find("fake-") == std::string::npos);
        REQUIRE(fake.requests().size() == 1);
        CHECK(fake.requests()[0].connect.empty());
    }
    SUBCASE("a user without its password sends an empty one, and is turned down: never anything directly") {
        test::OneShotServer server(old_banner().substr(20));
        const auto ended = log_in(fake.proxy("alice", ""), "127.0.0.1", server.port);
        CHECK(ended.kind == budget::Failure::fatal);
        CHECK(ended.what == "the proxy turned down the user name and password");
        REQUIRE(fake.requests().size() == 1);
        CHECK(fake.requests()[0].login == std::string("\x01\x05" "alice\x00", 8));
        CHECK_FALSE(server.accepted());
    }
}

TEST_CASE("proxy: SOCKS5 that wants no login gets none, even when grab has one") {
    test::FakeProxy fake(serving(Kind::socks5));
    CHECK(reached_server(log_in(fake.proxy("alice", "fake-secret"))));
    REQUIRE(fake.requests().size() == 1);
    CHECK(fake.requests()[0].greeting == std::string("\x05\x02\x00\x02", 4)); // offered
    CHECK(fake.requests()[0].login.empty());                                 // never sent
}

TEST_CASE("proxy: SOCKS5 that accepts none of grab's logins (FF), with and without a user") {
    test::ProxyScript script = serving(Kind::socks5);
    script.user = "alice"; // wants a login: FF to a greeting without one
    test::FakeProxy wants(script);
    const auto none = log_in(wants.proxy());
    CHECK(none.kind == budget::Failure::fatal);
    CHECK(none.what == "the proxy wants a user name and password");
    script.method = 0xFF;
    test::FakeProxy neither(script);
    const auto with = log_in(neither.proxy("alice", "fake-secret"));
    CHECK(with.kind == budget::Failure::fatal);
    CHECK(with.what == "the proxy takes none of the logins grab offers (none, or a user name and password)");
    CHECK(with.what.find("fake-") == std::string::npos);
}

TEST_CASE("proxy: each SOCKS5 refusal has its text and kind") {
    using budget::Failure;
    struct Reply {
        unsigned char rep;
        std::string host;
        Failure kind;
        std::optional<Failure> proxied;
        std::string_view what;
    };
    for (const Reply& r : {
             Reply{.rep = 0x01, .host = "x.invalid", .kind = Failure::other, .proxied = std::nullopt,
                   .what = "the proxy failed (its own error)"},
             Reply{.rep = 0x02, .host = "x.invalid", .kind = Failure::refused, .proxied = Failure::fatal,
                   .what = "the proxy doesn't allow connections to x.invalid:2222 (company proxies often allow only port 443)"},
             Reply{.rep = 0x03, .host = "x.invalid", .kind = Failure::other, .proxied = std::nullopt,
                   .what = "the proxy can't reach x.invalid (or couldn't look the name up)"},
             Reply{.rep = 0x04, .host = "x.invalid", .kind = Failure::other, .proxied = std::nullopt,
                   .what = "the proxy can't reach x.invalid (or couldn't look the name up)"},
             Reply{.rep = 0x04, .host = "192.0.2.7", .kind = Failure::other, .proxied = std::nullopt,
                   .what = "the proxy can't reach 192.0.2.7"},
             Reply{.rep = 0x05, .host = "x.invalid", .kind = Failure::other, .proxied = std::nullopt,
                   .what = "x.invalid refused the connection on port 2222 (is SSH on that port?)"},
             Reply{.rep = 0x06, .host = "x.invalid", .kind = Failure::other, .proxied = std::nullopt,
                   .what = "the proxy gave up reaching x.invalid:2222 (timed out)"},
             Reply{.rep = 0x07, .host = "x.invalid", .kind = Failure::fatal, .proxied = std::nullopt,
                   .what = "the proxy doesn't allow connections through it"},
             Reply{.rep = 0x08, .host = "x.invalid", .kind = Failure::fatal, .proxied = std::nullopt,
                   .what = "the proxy can't take a server name: write the server's IP address instead"},
             Reply{.rep = 0x08, .host = "2001:db8::7", .kind = Failure::fatal, .proxied = std::nullopt,
                   .what = "the proxy can't take an IPv6 address"},
             Reply{.rep = 0x09, .host = "x.invalid", .kind = Failure::other, .proxied = std::nullopt,
                   .what = "the proxy failed (its answer 09)"},
             Reply{.rep = 0xF0, .host = "x.invalid", .kind = Failure::fatal, .proxied = std::nullopt,
                   .what = "the onion service can't be found"},
             Reply{.rep = 0xF7, .host = "x.invalid", .kind = Failure::fatal, .proxied = std::nullopt,
                   .what = "the introduction to the onion service timed out"},
         }) {
        CAPTURE(static_cast<int>(r.rep));
        test::ProxyScript script;
        script.rep = r.rep;
        test::FakeProxy fake(script);
        const auto ended = log_in(fake.proxy(), r.host);
        CHECK(ended.kind == r.kind);
        CHECK(ended.proxied == r.proxied);
        CHECK(ended.what == r.what);
    }
}

TEST_CASE("proxy: SOCKS5's bound address as a name or IPv6, answers one byte at a time, or with the banner in one send") {
    for (const bool one_byte : {false, true}) {
        for (const std::string& bound : {std::string("\x01\0\0\0\0\0\0", 7), std::string("\x03\x09proxy.lan\x04\x38", 13),
                                         std::string("\x04", 1) + std::string(15, '\0') + std::string("\x01\x04\x38", 3)}) {
            CAPTURE(one_byte);
            CAPTURE(static_cast<int>(bound[0]));
            test::ProxyScript script = serving(Kind::socks5);
            script.bound = bound;
            script.one_byte = one_byte;
            test::FakeProxy fake(script);
            // Not a byte of the server's lost or taken for the proxy's.
            CHECK(reached_server(log_in(fake.proxy())));
        }
    }
}

TEST_CASE("proxy: SOCKS5 sends an address as one (ATYP 01, 04), a name as a name (03)") {
    test::FakeProxy fake(serving(Kind::socks5));
    CHECK(reached_server(log_in(fake.proxy(), "192.0.2.7", 22), "192.0.2.7"));
    CHECK(reached_server(log_in(fake.proxy(), "2001:db8::7", 22), "2001:db8::7"));
    CHECK(reached_server(log_in(fake.proxy(), "box.your-storagebox.de", 23), "box.your-storagebox.de"));
    const auto asked = fake.requests();
    REQUIRE(asked.size() == 3);
    CHECK(asked[0].connect == std::string("\x05\x01\x00\x01\xc0\x00\x02\x07\x00\x16", 10));
    CHECK(asked[1].connect == std::string("\x05\x01\x00\x04\x20\x01\x0d\xb8", 8) + std::string(11, '\0') +
                                  std::string("\x07\x00\x16", 3));
    CHECK(asked[2].connect == std::string("\x05\x01\x00\x03\x16" "box.your-storagebox.de\x00\x17", 29));
}

TEST_CASE("proxy: something other than SOCKS5 on the SOCKS5 port") {
    test::ProxyScript script;
    script.wrong = "HTTP/1.1 400 Bad Request\r\n\r\n";
    test::FakeProxy http(script);
    const auto said_http = log_in(http.proxy());
    CHECK(said_http.kind == budget::Failure::fatal);
    CHECK(said_http.what ==
          std::format("127.0.0.1:{} isn't a SOCKS5 proxy (it answers like an HTTP proxy: use http://)", http.port));
    script.wrong = std::string("\x04\x5a", 2);
    test::FakeProxy socks4(script);
    const auto said_4 = log_in(socks4.proxy());
    CHECK(said_4.kind == budget::Failure::fatal);
    CHECK(said_4.what == std::format("127.0.0.1:{} isn't a SOCKS5 proxy", socks4.port));
}

TEST_CASE("proxy: HTTP CONNECT without and with a Basic login") {
    test::OneShotServer server(old_banner().substr(20));
    test::ProxyScript script;
    script.kind = Kind::http;
    script.relay_port = server.port;
    test::FakeProxy fake(script);
    CHECK(reached_server(log_in(fake.proxy())));
    CHECK(server.accepted());
    test::ProxyScript login = serving(Kind::http);
    login.user = "alice";
    login.password = "fake-secret";
    test::FakeProxy basic(login);
    CHECK(reached_server(log_in(basic.proxy("alice", "fake-secret"), "2001:db8::7", 22), "2001:db8::7"));
    REQUIRE(fake.requests().size() == 1);
    CHECK(fake.requests()[0].header ==
          "CONNECT x.invalid:2222 HTTP/1.1\r\nHost: x.invalid:2222\r\nUser-Agent: grab/" GRAB_VERSION "\r\n\r\n");
    REQUIRE(basic.requests().size() == 1);
    CHECK(basic.requests()[0].header == "CONNECT [2001:db8::7]:22 HTTP/1.1\r\nHost: [2001:db8::7]:22\r\n"
                                        "Proxy-Authorization: Basic YWxpY2U6ZmFrZS1zZWNyZXQ=\r\n"
                                        "User-Agent: grab/" GRAB_VERSION "\r\n\r\n");
}

TEST_CASE("proxy: HTTP 407, with and without a user, and when only Windows sign-in would do") {
    test::ProxyScript script = serving(Kind::http);
    script.user = "alice";
    script.password = "fake-secret";
    {
        test::FakeProxy fake(script);
        const auto wrong = log_in(fake.proxy("alice", "fake-wrong"));
        CHECK(wrong.kind == budget::Failure::fatal);
        CHECK(wrong.what == "the proxy turned down the user name and password");
        CHECK(wrong.what.find("fake-") == std::string::npos);
        const auto none = log_in(fake.proxy());
        CHECK(none.kind == budget::Failure::fatal);
        CHECK(none.what == "the proxy wants a user name and password");
        CHECK(fake.requests().size() == 2); // one request each: nothing tried again
    }
    script.challenge = "Proxy-Authenticate: NTLM\r\nproxy-authenticate: Negotiate\r\n";
    {
        test::FakeProxy fake(script);
        const auto windows = log_in(fake.proxy("alice", "fake-secret2"));
        CHECK(windows.kind == budget::Failure::fatal);
        CHECK(windows.what == "the proxy wants Windows sign-in, which grab can't do yet; a helper such as Px on this PC "
                              "can do it for grab");
    }
    script.challenge = "Proxy-Authenticate: Digest realm=\"x, y\", qop=\"auth\"\r\n";
    {
        test::FakeProxy fake(script);
        const auto digest = log_in(fake.proxy("alice", "fake-secret2"));
        CHECK(digest.kind == budget::Failure::fatal);
        CHECK(digest.what == "the proxy wants a login grab can't do (Digest): grab logs in to HTTP proxies with Basic only");
    }
    // A proxy's long list is cut short: each scheme once, at most 4 named.
    std::string many = "Proxy-Authenticate: Digest, digest, DIGEST";
    for (int i = 0; i < 2000; ++i) many += std::format(", s{}", i);
    script.challenge = many + "\r\nProxy-Authenticate: Digest\r\n";
    {
        test::FakeProxy fake(script);
        const auto listed = log_in(fake.proxy("alice", "fake-secret2"));
        CHECK(listed.kind == budget::Failure::fatal);
        CHECK(listed.what ==
              "the proxy wants a login grab can't do (Digest, s0, s1, s2, ...): grab logs in to HTTP proxies with Basic only");
    }
}

TEST_CASE("proxy: each HTTP refusal has its text and kind; a redirect isn't followed") {
    using budget::Failure;
    struct Answer {
        std::string answer;
        Failure kind;
        std::optional<Failure> proxied;
        std::string what; // (one is built: a view of it would dangle)
    };
    for (const Answer& a : {
             Answer{.answer = "HTTP/1.1 403 Forbidden\r\n\r\n", .kind = Failure::refused, .proxied = Failure::fatal,
                    .what = "the proxy doesn't allow connections to x.invalid:2222 (company proxies often allow only port 443)"},
             Answer{.answer = "HTTP/1.1 405 Method Not Allowed\r\n\r\n", .kind = Failure::refused,
                    .proxied = Failure::fatal,
                    .what = "the proxy doesn't allow connections to x.invalid:2222 (company proxies often allow only port 443)"},
             Answer{.answer = "HTTP/1.1 502 Bad Gateway\r\n\r\n", .kind = Failure::other, .proxied = std::nullopt,
                    .what = "the proxy couldn't reach x.invalid:2222 (HTTP 502 Bad Gateway)"},
             Answer{.answer = "HTTP/1.1 503 Service Unavailable\r\nRetry-After: 5\r\n\r\n", .kind = Failure::other,
                    .proxied = std::nullopt,
                    .what = "the proxy couldn't reach x.invalid:2222 (HTTP 503 Service Unavailable)"},
             Answer{.answer = "HTTP/1.1 504 Gateway Timeout\r\n\r\n", .kind = Failure::other, .proxied = std::nullopt,
                    .what = "the proxy gave up reaching x.invalid:2222 (timed out)"},
             Answer{.answer = "HTTP/1.1 302 Found\r\nLocation: http://evil.example/\r\n\r\n", .kind = Failure::fatal,
                    .proxied = std::nullopt,
                    .what = "the proxy sent grab elsewhere (HTTP 302); grab doesn't follow that"},
             Answer{.answer = "HTTP/1.1 500 Internal Server Error\r\n\r\n", .kind = Failure::other,
                    .proxied = std::nullopt,
                    .what = "the proxy answered HTTP 500 Internal Server Error"},
             Answer{.answer = "HTTP/1.1 500\r\n\r\n", .kind = Failure::other, .proxied = std::nullopt,
                    .what = "the proxy answered HTTP 500"},
             // The proxy's words are shown without control characters (an escape, a bell), cut short.
             Answer{.answer = "HTTP/1.1 500 Bad\x1b[31m\x07 Thing" + std::string(200, 'z') + "\r\n\r\n",
                    .kind = Failure::other, .proxied = std::nullopt,
                    .what = "the proxy answered HTTP 500 Bad[31m Thing" + std::string(67, 'z')},
         }) {
        CAPTURE(a.answer.substr(0, 30));
        test::ProxyScript script;
        script.kind = Kind::http;
        script.answer = a.answer;
        test::FakeProxy fake(script);
        const auto ended = log_in(fake.proxy());
        CHECK(ended.kind == a.kind);
        CHECK(ended.proxied == a.proxied);
        CHECK(ended.what == a.what);
        CHECK(fake.requests().size() == 1);
    }
}

TEST_CASE("proxy: HTTP/1.0, bare LF, a Content-Length, one byte at a time: the tunnel starts right after the empty line") {
    for (const std::string& answer : {std::string("HTTP/1.0 200 Connection established\r\n\r\n"), std::string("HTTP/1.1 200 OK\n\n"),
                                      std::string("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nProxy-Agent: fake\r\n\r\n"),
                                      std::string("HTTP/1.1 204 No Content\r\nTransfer-Encoding: chunked\n\r\n"),
                                      // Interim answers before the final one (RFC 9110 15.2).
                                      std::string("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 Connection established\r\n\r\n"),
                                      std::string("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 102 Processing\nX: y\n\nHTTP/1.0 200 OK\r\n\r\n")}) {
        for (const bool one_byte : {false, true}) {
            CAPTURE(answer);
            CAPTURE(one_byte);
            test::ProxyScript script = serving(Kind::http);
            script.answer = answer;
            script.one_byte = one_byte;
            test::FakeProxy fake(script);
            CHECK(reached_server(log_in(fake.proxy())));
        }
    }
}

TEST_CASE("proxy: something other than an HTTP proxy's answer on the HTTP port") {
    struct Wrong {
        std::string bytes;
        std::string what;
    };
    for (const Wrong& w : {
             Wrong{.bytes = std::string("\x05\xff", 2),
                   .what = "isn't an HTTP proxy (it answers like a SOCKS proxy: use socks5://)"},
             Wrong{.bytes = std::string("\x00\x5b", 2),
                   .what = "isn't an HTTP proxy (it answers like a SOCKS proxy: use socks5://)"},
             Wrong{.bytes = "SSH-2.0-OpenSSH_9.6\r\n", .what = "isn't an HTTP proxy"},
             Wrong{.bytes = "HTTP/2 200\r\n\r\n", .what = "isn't an HTTP proxy"},
             Wrong{.bytes = "HTTP/1.1 OK\r\n\r\n", .what = "isn't an HTTP proxy"},
             // An answer too long for an HTTP proxy's header: over 100 lines, or over 16 KiB.
             Wrong{.bytes = "HTTP/1.1 200 OK\r\n" + [] {
                       std::string lines;
                       for (int i = 0; i < 101; ++i) lines += "X-Pad: 1\r\n";
                       return lines;
                   }() + "\r\n",
                   .what = "isn't an HTTP proxy (its answer is too long for one)"},
             Wrong{.bytes = "HTTP/1.1 200 OK\r\nX-Pad: " + std::string(17000, 'a') + "\r\n\r\n",
                   .what = "isn't an HTTP proxy (its answer is too long for one)"},
         }) {
        CAPTURE(w.bytes.substr(0, 20));
        test::ProxyScript script;
        script.kind = Kind::http;
        script.wrong = w.bytes;
        test::FakeProxy fake(script);
        const auto ended = log_in(fake.proxy());
        CHECK(ended.kind == budget::Failure::fatal);
        CHECK(ended.what == std::format("127.0.0.1:{} {}", fake.port, w.what));
    }
}

TEST_CASE("proxy: a request a proxy can't take never reaches it") {
    for (const Kind kind : {Kind::socks5, Kind::http}) {
        test::FakeProxy fake(serving(kind));
        CHECK(log_in(fake.proxy(), std::string(256, 'a')).what == "the server's name is longer than a proxy can take (255 bytes)");
        CHECK(log_in(fake.proxy(), "x.invalid\r\nHost: evil.example").what ==
              "the server's name can't hold spaces or control characters");
        CHECK(log_in(fake.proxy("al\nice", "")).what == "the proxy's user name can't hold spaces or control characters");
        CHECK(log_in(fake.proxy(), "fe80::1%eth0").what == "host fe80::1%eth0: an address with a zone can't go through a proxy");
        const auto long_secret = log_in(fake.proxy("alice", std::string(256, 'p')));
        if (kind == Kind::socks5) {
            CHECK(long_secret.what == "the proxy's password is longer than SOCKS5 allows (255 bytes)");
            CHECK(log_in(fake.proxy(std::string(256, 'u'), "")).what ==
                  "the proxy's user name is longer than SOCKS5 allows (255 bytes)");
            CHECK(fake.connections() == 0);
        } else {
            CHECK(reached_server(long_secret)); // Basic has no such limit
            CHECK(log_in(fake.proxy("al:ice", "fake-secret")).what ==
                  "the user name for an HTTP proxy can't hold ':' (Basic login)");
            CHECK(fake.connections() == 1);
        }
        CHECK(long_secret.what.find("ppp") == std::string::npos);
    }
}

TEST_CASE("proxy: a silent proxy ends at connect_timeout; closing grab ends its handshake at once") {
    struct Allow { // the rest of the tests connect again
        ~Allow() { ssh::allow_connections(); }
    } allow;
    for (const Kind kind : {Kind::socks5, Kind::http}) {
        test::ProxyScript script;
        script.kind = kind;
        script.silent = true;
        test::FakeProxy fake(script);
        const auto ended = log_in(fake.proxy(), "x.invalid", 2222, std::chrono::milliseconds(1'500));
        CHECK(ended.kind == budget::Failure::other);
        CHECK(ended.what == std::format("the proxy 127.0.0.1:{} didn't answer within 1.5 s", fake.port));
        CHECK(ended.took >= std::chrono::milliseconds(1'400));
        CHECK(ended.took < std::chrono::seconds(4));
        // connect_timeout off: the io timeout bounds the handshake instead.
        const auto off = log_in(fake.proxy(), "x.invalid", 2222, std::chrono::milliseconds(0), {},
                                std::chrono::milliseconds(1'500));
        CHECK(off.kind == budget::Failure::other);
        CHECK(off.what == std::format("the proxy 127.0.0.1:{} didn't answer within 1.5 s", fake.port));
        CHECK(off.took >= std::chrono::milliseconds(1'400));
        CHECK(off.took < std::chrono::seconds(4));

        std::atomic<bool> done{false};
        Ended cut;
        std::jthread login([&] {
            cut = log_in(fake.proxy(), "x.invalid", 2222, std::chrono::seconds(60));
            done = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        REQUIRE_FALSE(done.load()); // in the proxy's handshake
        const auto closed = std::chrono::steady_clock::now();
        ssh::close_all_connections();
        login.join();
        const auto took = std::chrono::steady_clock::now() - closed;
        CHECK(took < std::chrono::seconds(1));
        CHECK(cut.what == ssh::closing_text);
        CHECK(cut.kind == budget::Failure::fatal);
        ssh::allow_connections();
    }
}

TEST_CASE("proxy: a proxy by its IPv6 address; bind is checked against the proxy's addresses, not the server's") {
    test::FakeProxy v6(serving(Kind::socks5), true);
    const proxy::Proxy at = v6.proxy();
    CHECK(at.host == "::1");
    CHECK(proxy::text(at) == std::format("socks5://[::1]:{}", v6.port));
    CHECK(reached_server(log_in(at)));
    CHECK(reached_server(log_in(at, "x.invalid", 2222, std::chrono::seconds(10), "::1")));

    test::FakeProxy v4(serving(Kind::http));
    const auto mismatch = log_in(v4.proxy(), "x.invalid", 2222, std::chrono::seconds(10), "::1");
    CHECK(mismatch.kind == budget::Failure::fatal);
    CHECK(mismatch.what == "::1 (--bind) and the proxy 127.0.0.1 have no address of the same kind (IPv4, IPv6)");
    // An IPv6 server from an IPv4 bind address: the proxy is IPv4, and it is the one dialled.
    CHECK(reached_server(log_in(v4.proxy(), "2001:db8::7", 22, std::chrono::seconds(10), "127.0.0.1"), "2001:db8::7"));
    CHECK(v4.connections() == 1);
}

TEST_CASE("proxy: fail closed: a dead or unknown proxy fails the connection and nothing goes directly") {
    test::OneShotServer server(old_banner().substr(20));
    const int nobody = dead_port();
    for (const Kind kind : {Kind::socks5, Kind::http}) {
        const proxy::Proxy dead{.kind = kind, .host = "127.0.0.1", .port = nobody, .user = "", .password = ""};
        const auto refused = log_in(dead, "127.0.0.1", server.port);
        CHECK(refused.kind == budget::Failure::other);
        CHECK(refused.what == std::format("can't reach the proxy 127.0.0.1:{}: connection refused (is it running?)", nobody));
        if (kind == Kind::socks5) { // (each refusal takes Windows a second or two)
            const auto from = log_in(dead, "127.0.0.1", server.port, std::chrono::seconds(10), "127.0.0.1");
            CHECK(from.what == std::format("can't reach the proxy 127.0.0.1:{} from 127.0.0.1 (--bind): connection refused "
                                           "(is it running?)",
                                           nobody));
        }
        const proxy::Proxy unknown{.kind = kind, .host = "proxy.invalid", .port = 1080, .user = "", .password = ""};
        const auto lost = log_in(unknown, "127.0.0.1", server.port);
        CHECK(lost.kind == budget::Failure::other);
        CHECK(lost.what == "can't find the proxy proxy.invalid");
    }
    CHECK_FALSE(server.accepted());
}

TEST_CASE("proxy: a proxy closing before it answers, or the tunnel before the server says anything") {
    for (const Kind kind : {Kind::socks5, Kind::http}) {
        test::ProxyScript script;
        script.kind = kind;
        script.hang_up = true;
        test::FakeProxy hangs_up(script);
        const auto ended = log_in(hangs_up.proxy());
        CHECK(ended.kind == budget::Failure::refused);
        CHECK(ended.proxied == budget::Failure::other);
        CHECK(ended.what == std::format("the proxy 127.0.0.1:{} closed the connection without answering (is it a{} proxy?)",
                                        hangs_up.port, kind == Kind::socks5 ? " SOCKS5" : "n HTTP"));

        script.hang_up = false;
        script.close_after = true; // ssh -D answers success before it has reached the server
        test::FakeProxy drops(script);
        const auto dropped = log_in(drops.proxy());
        CHECK(dropped.kind == budget::Failure::refused);
        CHECK(dropped.proxied == budget::Failure::other);
        CHECK(dropped.what ==
              "the proxy connected, then the connection closed before x.invalid said anything: is the server up on port 2222?");
        // Trust's host-key scan, the first contact with a new server, says the same.
        const ssh::Reach reach{.bind = "", .bind_from = "--bind", .connect_timeout = std::nullopt, .proxy = drops.proxy()};
        const auto keys = ssh::scan_host_keys("x.invalid", 2222, std::chrono::seconds(10), {}, reach);
        REQUIRE_FALSE(keys);
        CHECK(keys.error() == "can't read the host keys of x.invalid:2222: the proxy connected, then the connection "
                              "closed before x.invalid said anything: is the server up on port 2222?");
    }
}

TEST_CASE("proxy: Trust's host-key scan goes through the proxy, every handshake") {
    for (const Kind kind : {Kind::socks5, Kind::http}) {
        test::FakeProxy fake(serving(kind));
        const ssh::Reach reach{.bind = "", .bind_from = "--bind", .connect_timeout = std::nullopt, .proxy = fake.proxy()};
        const auto keys = ssh::scan_host_keys("x.invalid", 2222, std::chrono::seconds(10), {}, reach);
        REQUIRE_FALSE(keys);
        MESSAGE(keys.error());
        // The server's KEXINIT came through the proxy: it was the key exchange that failed.
        CHECK(keys.error().starts_with("x.invalid:2222 has no host key grab can check"));
        const auto asked = fake.requests();
        REQUIRE(asked.size() == 1);
        if (kind == Kind::socks5) CHECK(asked[0].connect == std::string("\x05\x01\x00\x03\x09x.invalid\x08\xae", 16));
        else CHECK(asked[0].header.starts_with("CONNECT x.invalid:2222 HTTP/1.1\r\n"));
    }
    // A dead proxy: the scan fails with the proxy's reason, nothing direct.
    const proxy::Proxy dead{.kind = Kind::socks5, .host = "127.0.0.1", .port = dead_port(), .user = "", .password = ""};
    const ssh::Reach through{.bind = "", .bind_from = "--bind", .connect_timeout = std::nullopt, .proxy = dead};
    const auto keys = ssh::scan_host_keys("x.invalid", 2222, std::chrono::seconds(10), {}, through);
    REQUIRE_FALSE(keys);
    CHECK(keys.error().find("can't reach the proxy 127.0.0.1:") != std::string::npos);
}
#endif
