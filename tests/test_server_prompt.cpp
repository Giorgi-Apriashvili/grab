// The proxy questions of `grab server add` and `edit` (server_prompt.hpp; PROXIES.md decision 5),
// answered by a script instead of a console: what is asked, with which default, what is said
// when an answer can't be taken, and the proxy that comes out. Every secret is an obviously fake
// one.

#include "guard.hpp"
#include "proxy.hpp"
#include "server_ops.hpp"
#include "server_prompt.hpp"
#include "servers.hpp"

#include <doctest/doctest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace grab;
using proxy::Kind;

namespace {

// Answers in order, as server_cli's console gives them: trimmed, Enter (an empty answer) giving
// the default; secrets as typed. Running out is the end of the input.
class Script {
public:
    explicit Script(std::vector<std::string> answers, std::vector<std::string> secrets = {})
        : answers_(std::move(answers)), secrets_(std::move(secrets)) {}

    std::vector<std::string> asked; // "prompt [default]", or the prompt alone
    std::vector<std::string> secret_asked;
    std::vector<std::string> said;

    [[nodiscard]] server_prompt::Console console() {
        return server_prompt::Console{
            .ask = [this](std::string_view prompt, std::string_view def) -> std::optional<std::string> {
                asked.push_back(def.empty() ? std::string(prompt) : std::string(prompt) + " [" + std::string(def) + "]");
                if (next_ == answers_.size()) return std::nullopt;
                const std::string a = answers_[next_++];
                return a.empty() ? std::string(def) : a;
            },
            .secret = [this](std::string_view prompt) -> std::optional<std::string> {
                secret_asked.emplace_back(prompt);
                if (next_secret_ == secrets_.size()) return std::nullopt;
                return secrets_[next_secret_++];
            },
            .say = [this](std::string_view line) { said.emplace_back(line); },
        };
    }
    [[nodiscard]] std::optional<proxy::Proxy> run(const server_ops::ProxyOffer& offer, std::string_view host = "box.example") {
        servers::NewServer s;
        s.name = "Box";
        s.host = std::string(host);
        return server_prompt::ask_proxy(console(), offer, s);
    }

private:
    std::vector<std::string> answers_;
    std::vector<std::string> secrets_;
    std::size_t next_ = 0;
    std::size_t next_secret_ = 0;
};

proxy::Proxy socks(int port, std::string user = {}, std::string password = {}) {
    return proxy::Proxy{.kind = Kind::socks5, .host = "127.0.0.1", .port = port, .user = std::move(user),
                        .password = std::move(password)};
}

const server_ops::ProxyOffer directly{.proxy = proxy::Proxy{}, .from = guard::ProxyFrom::none, .password_saved = false,
                                      .why = {}};

server_ops::ProxyOffer offering(proxy::Proxy p, bool saved, guard::ProxyFrom from = guard::ProxyFrom::line) {
    return server_ops::ProxyOffer{.proxy = std::move(p), .from = from, .password_saved = saved, .why = {}};
}

constexpr std::string_view connect_question = "Connect  1) directly  2) through a SOCKS5 proxy  3) through an HTTP proxy";

} // namespace

TEST_CASE("server prompt: a new server connects directly with Enter; a proxy's user gets its password, not shown") {
    Script enter({""});
    CHECK(enter.run(directly) == proxy::Proxy{});
    CHECK(enter.asked == std::vector<std::string>{std::string(connect_question) + " [1]"});
    CHECK(enter.secret_asked.empty());

    Script with_login({"2", "127.0.0.1:9050", "alice"}, {"fake-proxy-pw"});
    CHECK(with_login.run(directly) == socks(9050, "alice", "fake-proxy-pw"));
    CHECK(with_login.asked == std::vector<std::string>{std::string(connect_question) + " [1]", "Proxy host:port",
                                                       "Proxy user name (Enter if none)"});
    CHECK(with_login.secret_asked == std::vector<std::string>{"Proxy password (not shown; Enter if none)"});
    CHECK(with_login.said.empty());

    // No user: no password asked; an IPv6 proxy in brackets.
    Script open({"3", "[::1]:3128", ""});
    CHECK(open.run(directly) == proxy::Proxy{.kind = Kind::http, .host = "::1", .port = 3128, .user = {}, .password = {}});
    CHECK(open.secret_asked.empty());
    // A user with no password: Enter is none.
    Script no_password({"2", "127.0.0.1:9050", "tor-circuit-1"}, {""});
    CHECK(no_password.run(directly) == socks(9050, "tor-circuit-1"));
}

TEST_CASE("server prompt: an answer grab can't take is asked again with the reason, never quoting it") {
    Script s({"4", "3", "socks5://fake-user:fake-pw@127.0.0.1:1080", "proxy.example", "proxy.example:3128", "fake:pw",
              "a b", "bob"},
             {"fake-proxy-pw"});
    CHECK(s.run(directly) == proxy::Proxy{.kind = Kind::http, .host = "proxy.example", .port = 3128, .user = "bob",
                                          .password = "fake-proxy-pw"});
    const std::string pasted = "  give the proxy's host and port alone, as 127.0.0.1:9050: its kind and user name are "
                               "asked on their own";
    CHECK(s.said == std::vector<std::string>{
                        "  1, 2 or 3",
                        pasted,
                        "  give the proxy's port too (127.0.0.1:9050)",
                        "  the user name for an HTTP proxy can't hold ':' (Basic login)",
                        "  the proxy's user name can't hold spaces or control characters",
                    });
    CHECK(s.asked.size() == 8);

    // rclone's "user:password" form at a SOCKS5 user: asked again, the answer not quoted.
    Script colon({"2", "127.0.0.1:1080", "fake-user:fake-pw", "alice"}, {"fake-proxy-pw"});
    CHECK(colon.run(directly) == socks(1080, "alice", "fake-proxy-pw"));
    CHECK(colon.said == std::vector<std::string>{"  a proxy user name can't hold ':' here: type the password at its own "
                                                 "question (the copy for older grabs can't hold it either)"});
    CHECK(colon.asked.size() == 4);

    // A SOCKS5 password over 255 bytes, typed again.
    Script long_password({"2", "127.0.0.1:1080", "alice"}, {std::string(256, 'x'), "fake-ok"});
    CHECK(long_password.run(directly) == socks(1080, "alice", "fake-ok"));
    CHECK(long_password.said == std::vector<std::string>{"  the proxy's password is longer than SOCKS5 allows (255 bytes)"});
    CHECK(long_password.secret_asked.size() == 2);
    // An HTTP proxy's Basic password has no such limit.
    Script basic({"3", "127.0.0.1:3128", "alice"}, {std::string(300, 'y')});
    CHECK(basic.run(directly).value_or(proxy::Proxy{}).password.size() == 300);

    // A host with a zone can't go through a proxy: only directly.
    Script zoned({"2", "1"});
    CHECK(zoned.run(directly, "fe80::1%eth0") == proxy::Proxy{});
    CHECK(zoned.said == std::vector<std::string>{
                            "  the host fe80::1%eth0 is an address with a zone, which can't go through a proxy: choose 1"});
}

TEST_CASE("server prompt: an edit offers the current proxy; Enter keeps it, and its password only for the same proxy and user") {
    const auto line = offering(socks(9050, "alice"), true);
    Script keep({"", "", ""}, {""});
    CHECK(keep.run(line) == socks(9050, "alice"));
    CHECK(keep.asked == std::vector<std::string>{std::string(connect_question) + " [2]", "Proxy host:port [127.0.0.1:9050]",
                                                 "Proxy user name (- for none) [alice]"});
    CHECK(keep.secret_asked == std::vector<std::string>{"Proxy password (not shown; Enter keeps the saved one)"});
    CHECK(keep.said.empty());
    // A new one typed.
    Script typed({"", "", ""}, {"fake-new-pw"});
    CHECK(typed.run(line) == socks(9050, "alice", "fake-new-pw"));

    // Another port, user or kind: the saved password isn't for it.
    for (const auto& [answers, want] : {
             std::pair{std::vector<std::string>{"", "127.0.0.1:9051", ""}, socks(9051, "alice")},
             std::pair{std::vector<std::string>{"", "", "bob"}, socks(9050, "bob")},
             std::pair{std::vector<std::string>{"3", "", ""},
                       proxy::Proxy{.kind = Kind::http, .host = "127.0.0.1", .port = 9050, .user = "alice", .password = {}}},
         }) {
        Script other(answers, {""});
        CHECK(other.run(line) == want);
        CHECK(other.secret_asked == std::vector<std::string>{"Proxy password (not shown; Enter if none)"});
    }
    // None saved: Enter is none.
    Script unsaved({"", "", ""}, {""});
    CHECK(unsaved.run(offering(socks(9050, "alice"), false)) == socks(9050, "alice"));
    CHECK(unsaved.secret_asked == std::vector<std::string>{"Proxy password (not shown; Enter if none)"});
    // "-" takes the user away, and with it the password.
    Script no_user({"", "", "-"});
    CHECK(no_user.run(line) == socks(9050));
    CHECK(no_user.secret_asked.empty());
    // Directly.
    Script direct({"1"});
    CHECK(direct.run(line) == proxy::Proxy{});
    // An HTTP one by its IPv6 address, offered as written.
    Script v6({"", "", ""});
    CHECK(v6.run(offering(proxy::Proxy{.kind = Kind::http, .host = "::1", .port = 3128, .user = {}, .password = {}}, false)) ==
          proxy::Proxy{.kind = Kind::http, .host = "::1", .port = 3128, .user = {}, .password = {}});
    CHECK(v6.asked == std::vector<std::string>{std::string(connect_question) + " [3]", "Proxy host:port [[::1]:3128]",
                                               "Proxy user name (Enter if none)"});
}

TEST_CASE("server prompt: a proxy kept in grab's store is offered and said; with nothing offered one has to be chosen") {
    Script kept({"", "", ""}, {""});
    CHECK(kept.run(offering(socks(9, "fake-user"), true, guard::ProxyFrom::kept)) == socks(9, "fake-user"));
    CHECK(kept.said == std::vector<std::string>{
                           "  Box goes through the SOCKS5 proxy fake-user@127.0.0.1:9, kept in grab's store (from rclone, or "
                           "a proxy line since removed): Enter keeps it, and the save writes it into grab.conf"});
    CHECK(kept.secret_asked == std::vector<std::string>{"Proxy password (not shown; Enter keeps the saved one)"});

    const server_ops::ProxyOffer none{.proxy = std::nullopt, .from = guard::ProxyFrom::none, .password_saved = false,
                                      .why = "[Box] has both a SOCKS and an HTTP proxy (...)"};
    Script choose({"", "1"});
    CHECK(choose.run(none) == proxy::Proxy{});
    CHECK(choose.said == std::vector<std::string>{"  [Box] has both a SOCKS and an HTTP proxy (...)", "  1, 2 or 3"});
    CHECK(choose.asked == std::vector<std::string>{std::string(connect_question), std::string(connect_question)});
    // Nothing offered: no address or user to keep either.
    Script fresh({"2", "127.0.0.1:1080", ""});
    CHECK(fresh.run(none) == socks(1080));
    CHECK(fresh.asked[1] == "Proxy host:port");
}

TEST_CASE("server prompt: the input running out at any question gives nothing") {
    const std::vector<std::string> all{"2", "127.0.0.1:9050", "alice"};
    for (std::size_t n = 0; n < all.size(); ++n) {
        CAPTURE(n);
        Script cut(std::vector<std::string>(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(n)));
        CHECK_FALSE(cut.run(directly).has_value());
    }
    Script no_secret(all);
    CHECK_FALSE(no_secret.run(directly).has_value());
    CHECK(no_secret.secret_asked.size() == 1);
}
