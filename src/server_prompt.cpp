#include "server_prompt.hpp"

#include "guard.hpp"
#include "server_conf.hpp"
#include "servers.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace grab::server_prompt {

namespace {

// Asks until `refused` takes the answer (nullopt); its reason is said each time it doesn't.
// nullopt when the input ran out.
template <class Refused>
std::optional<std::string> ask_until(const Console& io, std::string_view prompt, std::string_view def,
                                     Refused refused) {
    for (;;) {
        auto a = io.ask(prompt, def);
        if (!a) return std::nullopt;
        if (auto why = refused(*a)) {
            io.say("  " + *why);
            continue;
        }
        return a;
    }
}

} // namespace

std::optional<proxy::Proxy> ask_proxy(const Console& io, const server_ops::ProxyOffer& offer,
                                      const servers::NewServer& s) {
    using proxy::Kind;
    const std::string_view server = s.name;
    const std::string_view host = s.host;
    const proxy::Proxy* const offered = offer.proxy ? &*offer.proxy : nullptr;
    if (offered == nullptr && !offer.why.empty()) io.say("  " + offer.why);
    if (offered != nullptr && offer.from == guard::ProxyFrom::kept) {
        io.say(std::format("  {} goes through {}, kept in grab's store (from rclone, or a proxy line since removed): "
                           "Enter keeps it, and the save writes it into grab.conf",
                           server, proxy::said(*offered)));
    }
    const std::string_view was = offered == nullptr                ? ""
                                 : offered->kind == Kind::socks5 ? "2"
                                 : offered->kind == Kind::http   ? "3"
                                                                 : "1";
    auto how = ask_until(io, "Connect  1) directly  2) through a SOCKS5 proxy  3) through an HTTP proxy", was,
                         [&](const std::string& v) -> std::optional<std::string> {
                             if (v != "1" && v != "2" && v != "3") return "1, 2 or 3";
                             // A proxy takes the server's name or address as it is (3.8).
                             if (v != "1" && host.contains('%')) {
                                 return std::format("the host {} is an address with a zone, which can't go through a "
                                                    "proxy: choose 1",
                                                    host);
                             }
                             return std::nullopt;
                         });
    if (!how) return std::nullopt;
    if (*how == "1") return proxy::Proxy{};

    proxy::Proxy p;
    p.kind = *how == "2" ? Kind::socks5 : Kind::http;
    const bool had = offered != nullptr && offered->kind != Kind::none;
    // Each answer the check takes goes into `p`, so `p` holds the last one when the asking ends.
    auto address = ask_until(io, "Proxy host:port", had ? proxy::address(offered->host, offered->port) : "",
                             [&](const std::string& v) -> std::optional<std::string> {
                                 auto read = proxy::parse_address(v);
                                 if (!read) return read.error();
                                 p.host = std::move(read->host);
                                 p.port = read->port;
                                 return server_conf::check_proxy(p, server);
                             });
    if (!address) return std::nullopt;

    // An offered user is kept with Enter; "-" is none.
    const std::string_view had_user = had ? std::string_view(offered->user) : std::string_view();
    auto user = ask_until(io, had_user.empty() ? "Proxy user name (Enter if none)" : "Proxy user name (- for none)",
                          had_user, [&](const std::string& v) -> std::optional<std::string> {
                              p.user = v == "-" ? std::string() : v;
                              // rclone's "user:password" form: the password would land in grab.conf
                              // and in messages, and the copy for older grabs splits at the ':'
                              // (an HTTP user is refused it by check_proxy, for Basic login).
                              if (p.kind == Kind::socks5 && p.user.contains(':')) {
                                  return "a proxy user name can't hold ':' here: type the password at its own "
                                         "question (the copy for older grabs can't hold it either)";
                              }
                              return server_conf::check_proxy(p, server);
                          });
    if (!user) return std::nullopt;
    if (p.user.empty()) return p;

    // The saved password goes on only to the proxy and user it was typed for (as update_server
    // keeps it).
    const bool keeps = offer.password_saved && had && offered->kind == p.kind && offered->host == p.host &&
                       offered->port == p.port && offered->user == p.user;
    for (;;) {
        auto typed = io.secret(keeps ? "Proxy password (not shown; Enter keeps the saved one)"
                                     : "Proxy password (not shown; Enter if none)");
        if (!typed) return std::nullopt;
        if (p.kind == Kind::socks5 && typed->size() > 255) {
            std::ranges::fill(*typed, '\0');
            io.say("  the proxy's password is longer than SOCKS5 allows (255 bytes)");
            continue;
        }
        p.password = std::move(*typed);
        return p;
    }
}

} // namespace grab::server_prompt
