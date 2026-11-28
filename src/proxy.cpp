#include "proxy.hpp"

#include "conn_budget.hpp"
#include "ssh.hpp"
#include "util.hpp"

#ifdef _WIN32
#include <winsock2.h> // IWYU pragma: keep
#include <ws2tcpip.h> // IWYU pragma: keep
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <format>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifndef GRAB_VERSION
#define GRAB_VERSION "dev"
#endif

namespace grab::proxy {

namespace {

using budget::Failure;
using Clock = std::chrono::steady_clock;

// ---- the written forms ------------------------------------------------------------------------

constexpr std::string_view written_as = "write the proxy as socks5://host:port or http://host:port";

bool alnum(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
bool unreserved(char c) { return alnum(c) || c == '-' || c == '.' || c == '_' || c == '~'; }
bool host_char(char c) { return alnum(c) || c == '-' || c == '.' || c == '_'; }
bool hex_char(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
// A space, a control character or DEL: never in a name or a user sent to a proxy.
bool blank_or_control(char c) { return static_cast<unsigned char>(c) <= 0x20 || c == 0x7F; }
bool ascii(std::string_view s) {
    return std::ranges::all_of(s, [](char c) { return static_cast<unsigned char>(c) < 0x80; });
}

std::string percent_encode(std::string_view s) {
    std::string out;
    for (const char c : s) {
        if (unreserved(c)) out += c;
        else out += std::format("%{:02X}", static_cast<unsigned char>(c));
    }
    return out;
}

std::optional<std::string> percent_decode(std::string_view s) {
    auto value = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            out += s[i];
            continue;
        }
        if (i + 2 >= s.size() || !hex_char(s[i + 1]) || !hex_char(s[i + 2])) return std::nullopt;
        out += static_cast<char>(value(s[i + 1]) * 16 + value(s[i + 2]));
        i += 2;
    }
    return out;
}

// An IPv6 address as a proxy's host can be (no zone); getaddrinfo checks the rest.
bool ipv6_like(std::string_view s) {
    return s.contains(':') && s.size() <= 45 &&
           std::ranges::all_of(s, [](char c) { return hex_char(c) || c == ':' || c == '.'; });
}

struct HostPort {
    std::string host;
    std::optional<int> port; // none written
};

// "host:port", "[v6]:port", or either without the port; `example` shows the form in reasons.
std::expected<HostPort, std::string> split_host_port(std::string_view s, std::string_view example) {
    if (s.empty()) return util::failf("give the proxy's host and port ({})", example);
    HostPort out;
    std::string_view port_text;
    bool has_port = false;
    if (s.front() == '[') {
        const auto close = s.find(']');
        if (close == std::string_view::npos) return util::fail("a ']' is missing after the proxy's IPv6 address");
        out.host = std::string(s.substr(1, close - 1));
        if (!ipv6_like(out.host)) return util::fail("the proxy's address in brackets isn't an IPv6 address");
        const auto after = s.substr(close + 1);
        if (!after.empty() && after.front() != ':') return util::failf("write the proxy as {}", example);
        has_port = !after.empty();
        if (has_port) port_text = after.substr(1);
    } else {
        const auto colon = s.find(':');
        if (colon != std::string_view::npos && s.find(':', colon + 1) != std::string_view::npos)
            return util::fail("write the proxy's IPv6 address in brackets: [::1]:1080");
        out.host = std::string(s.substr(0, colon));
        if (out.host.empty() || out.host.front() == '-' || out.host.front() == '.' ||
            !std::ranges::all_of(out.host, host_char))
            return util::fail("use a host name (letters, digits, - and .) or an IP address for the proxy");
        has_port = colon != std::string_view::npos;
        if (has_port) port_text = s.substr(colon + 1);
    }
    if (!has_port) return out;
    int port = 0;
    const auto [end, ec] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (port_text.empty() || ec != std::errc() || end != port_text.data() + port_text.size() || port < 1 ||
        port > 65535)
        return util::fail("the proxy's port must be from 1 to 65535");
    out.port = port;
    return out;
}

// What can't be in a proxy's user name (checked as parse reads one, and again before it is sent).
std::optional<std::string> check_user(Kind kind, std::string_view user) {
    if (std::ranges::any_of(user, blank_or_control)) return "the proxy's user name can't hold spaces or control characters";
    if (kind == Kind::http && user.contains(':')) return "the user name for an HTTP proxy can't hold ':' (Basic login)";
    if (kind == Kind::socks5 && user.size() > 255) return "the proxy's user name is longer than SOCKS5 allows (255 bytes)";
    return std::nullopt;
}

// The text of a proxy's own words (an HTTP reason phrase, a login scheme), which grab shows:
// printable ASCII only, at most 80 characters.
std::string shown(std::string_view s) {
    std::string out;
    for (const char c : s) {
        if (c >= 0x20 && c < 0x7F && out.size() < 80) out += c;
    }
    return std::string(util::trim(out));
}

// ---- the handshakes ---------------------------------------------------------------------------

#ifdef _WIN32
using socket_t = SOCKET;
int last_error() { return WSAGetLastError(); }
bool would_block(int e) { return e == WSAEWOULDBLOCK || e == WSAEINTR; }
constexpr int send_flags = 0;
#else
using socket_t = int;
int last_error() { return errno; }
bool would_block(int e) { return e == EAGAIN || e == EWOULDBLOCK || e == EINTR; }
constexpr int send_flags = MSG_NOSIGNAL;
#endif

// Bytes that hold a secret (a SOCKS5 login, a Basic credential), zeroed when they go.
struct Secret {
    std::string bytes;
    Secret() = default;
    explicit Secret(std::string b) : bytes(std::move(b)) {}
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
    ~Secret() { std::ranges::fill(bytes, '\0'); }
};

// The socket to the proxy during its handshake: whole sends and exact reads, each wait 250 ms at
// a time against the Wait's deadline.
class Line {
public:
    Line(socket_t s, const Wait& wait, std::string where) : s_(s), wait_(wait), where_(std::move(where)) {}

    [[nodiscard]] const std::string& where() const { return where_; }

    void send(std::string_view bytes) {
        while (!bytes.empty()) {
            const int n = ::send(s_, bytes.data(), static_cast<int>(std::min<std::size_t>(bytes.size(), 65536)), send_flags);
            if (n > 0) {
                bytes.remove_prefix(static_cast<std::size_t>(n));
                continue;
            }
            if (n < 0 && would_block(last_error())) {
                await(true);
                continue;
            }
            closed("before it answered"); // a reset
        }
    }

    // Exactly `n` bytes, never one more: what follows may be the server's. `closed_how` ends "the
    // proxy ... closed the connection " when it closes first.
    std::string read(std::size_t n, std::string_view closed_how) {
        std::string out(n, '\0');
        std::size_t got = 0;
        while (got < n) {
            const int r = ::recv(s_, out.data() + got, static_cast<int>(n - got), 0);
            if (r > 0) {
                got += static_cast<std::size_t>(r);
                continue;
            }
            if (r < 0 && would_block(last_error())) {
                await(false);
                continue;
            }
            closed(closed_how);
        }
        return out;
    }

    // The proxy closed or reset the connection before its answer was whole. A proxy that caps its
    // connections may do that, and so may one that isn't of this kind: `refused` while others
    // are open, else other (PROXIES.md 3.4).
    [[noreturn]] void closed(std::string_view how) const {
        if (cancelled()) throw ssh::Error(Failure::fatal, "cancelled");
        throw ssh::Error(Failure::refused, std::format("the proxy {} closed the connection {}", where_, how), Failure::other);
    }

private:
    [[nodiscard]] bool cancelled() const { return wait_.cancelled && wait_.cancelled(); }

    // Until the socket can be read (or written), the deadline passes or the wait is cancelled.
    void await(bool write) const {
        for (;;) {
            if (cancelled()) throw ssh::Error(Failure::fatal, "cancelled");
            auto slice = std::chrono::milliseconds(250);
            if (wait_.limit.count() > 0) {
                const auto left =
                    std::chrono::duration_cast<std::chrono::milliseconds>(wait_.started + wait_.limit - Clock::now());
                if (left.count() <= 0) {
                    throw ssh::Error(Failure::other,
                                     std::format("the proxy {} didn't answer within {}", where_, say_time(wait_.limit)));
                }
                slice = std::min(slice, left);
            }
            fd_set ready;
            fd_set failed;
            FD_ZERO(&ready);
            FD_ZERO(&failed);
            FD_SET(s_, &ready);
            FD_SET(s_, &failed);
            timeval tv{.tv_sec = 0, .tv_usec = static_cast<long>(slice.count() * 1000)};
            const int n = select(static_cast<int>(s_ + 1), write ? nullptr : &ready, write ? &ready : nullptr, &failed, &tv);
            if (n > 0) return;
            if (n < 0) {
                const int e = last_error();
                if (cancelled()) throw ssh::Error(Failure::fatal, "cancelled");
                throw ssh::Error(Failure::other, std::format("the connection to the proxy {} failed ({})", where_, e));
            }
        }
    }

    socket_t s_;
    const Wait& wait_;
    std::string where_;
};

// Tor's own SOCKS5 replies for onion services (torspec, socks-extensions: ExtendedErrors).
std::optional<std::string_view> tor_reply(unsigned char rep) {
    switch (rep) {
    case 0xF0: return "the onion service can't be found";
    case 0xF1: return "the onion service's descriptor is invalid";
    case 0xF2: return "the introduction to the onion service failed";
    case 0xF3: return "the rendezvous with the onion service failed";
    case 0xF4: return "the onion service wants client authorization (set it up in Tor)";
    case 0xF5: return "the onion service turned down Tor's client authorization";
    case 0xF6: return "the onion address is invalid";
    case 0xF7: return "the introduction to the onion service timed out";
    default: return std::nullopt;
    }
}

// A SOCKS5 CONNECT reply other than success.
[[noreturn]] void socks_refusal(unsigned char rep, unsigned char atyp, std::string_view host, int port) {
    const std::string hp = address(host, port);
    switch (rep) {
    case 0x01: throw ssh::Error(Failure::other, "the proxy failed (its own error)");
    case 0x02:
        throw ssh::Error(Failure::refused,
                         std::format("the proxy doesn't allow connections to {} (company proxies often allow only port 443)", hp),
                         Failure::fatal);
    case 0x03:
    case 0x04:
        throw ssh::Error(Failure::other, atyp == 0x03
                                             ? std::format("the proxy can't reach {} (or couldn't look the name up)", host)
                                             : std::format("the proxy can't reach {}", host));
    case 0x05:
        throw ssh::Error(Failure::other, std::format("{} refused the connection on port {} (is SSH on that port?)", host, port));
    case 0x06: throw ssh::Error(Failure::other, std::format("the proxy gave up reaching {} (timed out)", hp));
    case 0x07: throw ssh::Error(Failure::fatal, "the proxy doesn't allow connections through it");
    case 0x08:
        throw ssh::Error(Failure::fatal, atyp == 0x03 ? std::string("the proxy can't take a server name: write the server's IP "
                                                                    "address instead")
                                         : atyp == 0x04 ? std::string("the proxy can't take an IPv6 address")
                                                        : std::string("the proxy can't take an IPv4 address"));
    default:
        if (const auto tor = tor_reply(rep)) throw ssh::Error(Failure::fatal, std::string(*tor));
        throw ssh::Error(Failure::other, std::format("the proxy failed (its answer {:02X})", rep));
    }
}

[[noreturn]] void not_socks5(const Line& line, std::string_view answer) {
    if (answer.starts_with('H')) {
        throw ssh::Error(Failure::fatal,
                         std::format("{} isn't a SOCKS5 proxy (it answers like an HTTP proxy: use http://)", line.where()));
    }
    throw ssh::Error(Failure::fatal, std::format("{} isn't a SOCKS5 proxy", line.where()));
}

bool same_ignoring_case(std::string_view a, std::string_view b) { return util::to_lower(a) == util::to_lower(b); }

// The login schemes a Proxy-Authenticate value offers ("Basic realm=\"x\", NTLM" -> Basic, NTLM):
// each challenge's first token; a token followed by '=' is a parameter.
void schemes_of(std::string_view value, std::vector<std::string>& out) {
    for (const auto& piece : util::split(value, ',')) {
        std::string_view p = piece;
        const auto end = std::min(p.find_first_of(" ="), p.size());
        const auto token = p.substr(0, end);
        if (token.empty() || (end < p.size() && p[end] == '=')) continue;
        if (!std::ranges::all_of(token, [](char c) { return alnum(c) || c == '-' || c == '_' || c == '.'; })) continue;
        std::string scheme = shown(token);
        if (std::ranges::none_of(out, [&](const std::string& s) { return same_ignoring_case(s, scheme); }))
            out.push_back(std::move(scheme));
    }
}

// An HTTP answer other than 2xx.
[[noreturn]] void http_refusal(int code, std::string_view reason, const std::vector<std::string>& schemes, bool login,
                               std::string_view host, int port) {
    const std::string hp = address(host, port);
    const std::string status = reason.empty() ? std::format("HTTP {}", code) : std::format("HTTP {} {}", code, reason);
    if (code >= 300 && code < 400)
        throw ssh::Error(Failure::fatal, std::format("the proxy sent grab elsewhere (HTTP {}); grab doesn't follow that", code));
    if (code == 403 || code == 405) {
        throw ssh::Error(Failure::refused,
                         std::format("the proxy doesn't allow connections to {} (company proxies often allow only port 443)", hp),
                         Failure::fatal);
    }
    if (code == 407) {
        const bool basic = std::ranges::any_of(schemes, [](const std::string& s) { return same_ignoring_case(s, "Basic"); });
        const bool windows = !schemes.empty() && std::ranges::all_of(schemes, [](const std::string& s) {
            return same_ignoring_case(s, "NTLM") || same_ignoring_case(s, "Negotiate") || same_ignoring_case(s, "Kerberos");
        });
        if (windows) {
            throw ssh::Error(Failure::fatal, "the proxy wants Windows sign-in, which grab can't do yet; a helper such as Px on "
                                             "this PC can do it for grab");
        }
        if (!basic && !schemes.empty()) {
            // The proxy's words, so at most 4 of its schemes (PROXIES.md 3.2).
            constexpr std::size_t most = 4;
            const std::vector<std::string> named(schemes.begin(), schemes.begin() + static_cast<std::ptrdiff_t>(std::min(schemes.size(), most)));
            throw ssh::Error(Failure::fatal, std::format("the proxy wants a login grab can't do ({}{}): grab logs in to HTTP "
                                                         "proxies with Basic only",
                                                         util::join(named, ", "), schemes.size() > most ? ", ..." : ""));
        }
        throw ssh::Error(Failure::fatal, login ? "the proxy turned down the user name and password"
                                               : "the proxy wants a user name and password");
    }
    if (code == 502 || code == 503)
        throw ssh::Error(Failure::other, std::format("the proxy couldn't reach {} ({})", hp, status));
    if (code == 504) throw ssh::Error(Failure::other, std::format("the proxy gave up reaching {} (timed out)", hp));
    throw ssh::Error(Failure::other, std::format("the proxy answered {}", status));
}

} // namespace

// ---- the written forms ------------------------------------------------------------------------

std::expected<Proxy, std::string> parse(std::string_view text, std::string_view server) {
    const std::string_view t = util::trim(text);
    const auto sep = t.find("://");
    if (t.empty() || sep == std::string_view::npos) return util::fail(std::string(written_as));
    const std::string scheme = util::to_lower(t.substr(0, sep));
    Proxy p;
    std::string_view example;
    if (scheme == "socks5" || scheme == "socks5h") {
        p.kind = Kind::socks5;
        example = "socks5://host:1080";
    } else if (scheme == "http") {
        p.kind = Kind::http;
        example = "http://host:3128";
    } else if (scheme == "socks4" || scheme == "socks4a") {
        return util::fail("grab can't use SOCKS4 proxies yet: use socks5://");
    } else if (scheme == "https") {
        return util::fail("grab can't use HTTPS proxies yet: use http://");
    } else if (scheme == "socks") {
        return util::fail("say which SOCKS: grab can use socks5://");
    } else if (!scheme.empty() && scheme.size() <= 10 && scheme.front() >= 'a' && scheme.front() <= 'z' &&
               std::ranges::all_of(scheme, [](char c) { return alnum(c) || c == '+' || c == '-' || c == '.'; })) {
        return util::failf("grab can use socks5:// and http:// proxies, not {}://", scheme);
    } else {
        // Not a scheme (RFC 3986): what is before :// may be a login, never repeated.
        return util::fail(std::string(written_as));
    }
    std::string_view rest = t.substr(sep + 3);
    if (rest.ends_with('/')) rest.remove_suffix(1);
    if (rest.find_first_of("/?#") != std::string_view::npos)
        return util::failf("a proxy's address has no path: write it as {}", example);
    if (const auto at = rest.rfind('@'); at != std::string_view::npos) {
        const auto userinfo = rest.substr(0, at);
        if (userinfo.contains(':')) {
            return util::failf("keep the proxy's password in grab's store, not grab.conf: take it out of the line, then "
                               "Edit {} and type it there",
                               server);
        }
        if (userinfo.empty()) return util::fail("give the proxy's user name before @, or leave the @ out");
        auto user = percent_decode(userinfo);
        if (!user) return util::fail("the proxy's user name has a stray %: write % as %25");
        if (auto bad = check_user(p.kind, *user)) return util::fail(std::move(*bad));
        p.user = std::move(*user);
        rest = rest.substr(at + 1);
    }
    auto hp = split_host_port(rest, example);
    if (!hp) return util::fail(std::move(hp.error()));
    if (!hp->port) return util::failf("give the proxy's port ({})", example);
    p.host = std::move(hp->host);
    p.port = *hp->port;
    return p;
}

std::expected<Proxy, std::string> parse_rclone_socks(std::string_view text) {
    Proxy p{.kind = Kind::socks5, .host = {}, .port = 0, .user = {}, .password = {}};
    std::string_view host_port = text;
    // As lib/proxy's SOCKS5Dial: strings.SplitN(addr, "@", 2), then the part before it at its
    // first ':' into user and password.
    if (const auto at = text.find('@'); at != std::string_view::npos) {
        const auto creds = text.substr(0, at);
        const auto colon = creds.find(':');
        p.user = std::string(creds.substr(0, colon));
        if (colon != std::string_view::npos) p.password = std::string(creds.substr(colon + 1));
        host_port = text.substr(at + 1);
        if (p.user.empty()) return util::fail("the proxy's user name before @ is empty");
        if (auto bad = check_user(p.kind, p.user)) return util::fail(std::move(*bad));
        if (p.password.size() > 255) return util::fail("the proxy's password is longer than SOCKS5 allows (255 bytes)");
    }
    auto hp = split_host_port(host_port, "host:1080");
    if (!hp) return util::fail(std::move(hp.error()));
    if (!hp->port) return util::fail("give the proxy's port (host:1080)");
    p.host = std::move(hp->host);
    p.port = *hp->port;
    return p;
}

std::expected<Proxy, std::string> parse_rclone_http(std::string_view text) {
    const auto sep = text.find("://");
    const std::string scheme = util::to_lower(text.substr(0, sep == std::string_view::npos ? 0 : sep));
    if (scheme == "https") return util::fail("grab can't use HTTPS proxies yet: use http://");
    if (scheme != "http") return util::fail("an HTTP proxy's address starts with http://");
    // rclone used only the URL's host and user: a path or query after it changed nothing.
    std::string_view authority = text.substr(sep + 3);
    authority = authority.substr(0, std::min(authority.find_first_of("/?#"), authority.size()));
    Proxy p{.kind = Kind::http, .host = {}, .port = 0, .user = {}, .password = {}};
    // As Go's url.Parse: the user information ends at the last '@', the user at its first ':'.
    if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
        const auto userinfo = authority.substr(0, at);
        const auto colon = userinfo.find(':');
        auto user = percent_decode(userinfo.substr(0, colon));
        auto password = percent_decode(colon == std::string_view::npos ? std::string_view() : userinfo.substr(colon + 1));
        if (!user || !password) return util::fail("the proxy's user name or password has a stray %");
        if (user->empty()) return util::fail("the proxy's user name before @ is empty");
        if (auto bad = check_user(p.kind, *user)) return util::fail(std::move(*bad));
        p.user = std::move(*user);
        p.password = std::move(*password);
        authority = authority.substr(at + 1);
    }
    auto hp = split_host_port(authority, "http://host:3128");
    if (!hp) return util::fail(std::move(hp.error()));
    p.host = std::move(hp->host);
    p.port = hp->port.value_or(80); // as rclone's HTTPConnectDial
    return p;
}

std::string text(const Proxy& proxy) {
    if (proxy.kind == Kind::none) return {};
    const std::string user = proxy.user.empty() ? std::string() : percent_encode(proxy.user) + "@";
    return std::format("{}://{}{}", proxy.kind == Kind::socks5 ? "socks5" : "http", user, address(proxy.host, proxy.port));
}

std::string address(std::string_view host, int port) {
    return host.contains(':') ? std::format("[{}]:{}", host, port) : std::format("{}:{}", host, port);
}

std::string_view kind_name(Kind kind) {
    switch (kind) {
    case Kind::socks5: return "SOCKS5";
    case Kind::http: return "HTTP";
    case Kind::none: break;
    }
    return {};
}

std::optional<std::string> check_request(const Proxy& proxy, std::string_view host, int port) {
    if (proxy.kind == Kind::none) return std::nullopt;
    if (proxy.host.empty()) return "the proxy's host is missing";
    if (proxy.port < 1 || proxy.port > 65535) return "the proxy's port must be from 1 to 65535";
    if (host.empty()) return "the server's host is missing";
    if (std::ranges::any_of(host, blank_or_control)) return "the server's name can't hold spaces or control characters";
    if (!ascii(host)) return std::format("write the server's name {} in its xn-- form: a proxy takes only ASCII names", host);
    if (host.size() > 255) return "the server's name is longer than a proxy can take (255 bytes)";
    if (host.contains('%')) return std::format("host {}: an address with a zone can't go through a proxy", host);
    if (port < 1 || port > 65535) return "the server's port must be from 1 to 65535";
    if (proxy.user.empty()) return std::nullopt;
    if (auto bad = check_user(proxy.kind, proxy.user)) return bad;
    if (proxy.kind == Kind::socks5 && proxy.password.size() > 255)
        return "the proxy's password is longer than SOCKS5 allows (255 bytes)";
    return std::nullopt;
}

std::string say_time(std::chrono::milliseconds t) {
    const auto ms = t.count();
    if (ms >= 60'000 && ms % 60'000 == 0) return std::format("{} min", ms / 60'000);
    if (ms >= 1'000 && ms % 1'000 == 0) return std::format("{} s", ms / 1'000);
    if (ms >= 1'000) return std::format("{:g} s", static_cast<double>(ms) / 1000.0);
    return std::format("{} ms", ms);
}

// ---- the handshakes ---------------------------------------------------------------------------

void socks5_connect(std::uintptr_t socket, const Proxy& proxy, std::string_view host, int port, const Wait& wait) {
    if (auto bad = check_request(proxy, host, port)) throw ssh::Error(Failure::fatal, *bad);
    Line line(static_cast<socket_t>(socket), wait, address(proxy.host, proxy.port));
    const bool login = !proxy.user.empty();
    // No login offered, and with a user also RFC 1929's (02); never GSSAPI (01).
    line.send(login ? std::string_view("\x05\x02\x00\x02", 4) : std::string_view("\x05\x01\x00", 3));
    const std::string chosen = line.read(2, "without answering (is it a SOCKS5 proxy?)");
    if (chosen[0] != '\x05') not_socks5(line, chosen);
    const auto method = static_cast<unsigned char>(chosen[1]);
    if (method == 0xFF) {
        throw ssh::Error(Failure::fatal, login ? "the proxy takes none of the logins grab offers (none, or a user name and "
                                                 "password)"
                                               : "the proxy wants a user name and password");
    }
    if (method == 0x02 && login) {
        // A user without a password sends an empty one (Tor's way of keeping streams apart).
        // Reserved first, so that no copy of the password is left behind by a reallocation.
        Secret request;
        request.bytes.reserve(3 + proxy.user.size() + proxy.password.size());
        request.bytes += '\x01';
        request.bytes += static_cast<char>(proxy.user.size());
        request.bytes += proxy.user;
        request.bytes += static_cast<char>(proxy.password.size());
        request.bytes += proxy.password;
        line.send(request.bytes);
        const std::string status = line.read(2, "during the login");
        if (status[1] != '\0') throw ssh::Error(Failure::fatal, "the proxy turned down the user name and password");
    } else if (method != 0x00) {
        throw ssh::Error(Failure::fatal, std::format("{} chose a login grab didn't offer ({:02X})", line.where(), method));
    }

    // The server as written: an address as one (01, 04), a name as a name (03), which the proxy
    // looks up. Nothing is resolved here.
    std::string request("\x05\x01\x00", 3);
    in_addr v4{};
    in6_addr v6{};
    const std::string name(host);
    unsigned char atyp = 0x03;
    if (inet_pton(AF_INET, name.c_str(), &v4) == 1) {
        atyp = 0x01;
        request += '\x01';
        request.append(reinterpret_cast<const char*>(&v4), 4);
    } else if (inet_pton(AF_INET6, name.c_str(), &v6) == 1) {
        atyp = 0x04;
        request += '\x04';
        request.append(reinterpret_cast<const char*>(&v6), 16);
    } else {
        request += '\x03';
        request += static_cast<char>(name.size());
        request += name;
    }
    request += static_cast<char>((port >> 8) & 0xFF);
    request += static_cast<char>(port & 0xFF);
    line.send(request);
    const std::string asked = std::format("before it answered grab's request for {}", address(host, port));
    const std::string head = line.read(4, asked);
    if (head[0] != '\x05') not_socks5(line, head);
    if (head[1] != '\0') socks_refusal(static_cast<unsigned char>(head[1]), atyp, host, port);
    // The address it connected from (ignored), read to its last byte: the server's come next.
    std::size_t bound = 0;
    switch (head[3]) {
    case '\x01': bound = 4; break;
    case '\x04': bound = 16; break;
    case '\x03': bound = static_cast<unsigned char>(line.read(1, asked)[0]); break;
    default:
        throw ssh::Error(Failure::fatal,
                         std::format("{} isn't a SOCKS5 proxy (its answer has the address type {:02X})", line.where(),
                                     static_cast<unsigned char>(head[3])));
    }
    (void)line.read(bound + 2, asked);
}

void http_connect(std::uintptr_t socket, const Proxy& proxy, std::string_view host, int port, const Wait& wait) {
    if (auto bad = check_request(proxy, host, port)) throw ssh::Error(Failure::fatal, *bad);
    Line line(static_cast<socket_t>(socket), wait, address(proxy.host, proxy.port));
    const bool login = !proxy.user.empty();
    const std::string hp = address(host, port);
    {
        // The login goes with the first request: no 407 round trip. Each buffer holding it is
        // reserved first, so that no copy is left behind by a reallocation.
        Secret request;
        request.bytes.reserve(256 + 2 * hp.size() + 2 * (proxy.user.size() + proxy.password.size()));
        request.bytes += std::format("CONNECT {} HTTP/1.1\r\nHost: {}\r\n", hp, hp);
        if (login) {
            Secret credential;
            credential.bytes.reserve(proxy.user.size() + 1 + proxy.password.size());
            credential.bytes += proxy.user;
            credential.bytes += ':';
            credential.bytes += proxy.password;
            const Secret encoded(util::base64_encode(credential.bytes));
            request.bytes += "Proxy-Authorization: Basic ";
            request.bytes += encoded.bytes;
            request.bytes += "\r\n";
        }
        request.bytes += "User-Agent: grab/" GRAB_VERSION "\r\n\r\n";
        line.send(request.bytes);
    }

    // The answer's header byte by byte to its empty line: what comes after it is the server's.
    // Bare LF ends a line too; at most 16 KiB and 100 lines.
    constexpr std::size_t most_bytes = std::size_t{16} * 1024;
    constexpr std::size_t most_lines = 100;
    const std::string too_long = std::format("{} isn't an HTTP proxy (its answer is too long for one)", line.where());
    // An interim answer (1xx, not 101) comes before the final one (RFC 9110 15.2): the caps hold
    // across all of them.
    std::vector<std::string> lines;
    std::string current;
    std::string_view status;
    int code = 0;
    std::size_t total = 0;
    std::size_t lines_seen = 0;
    for (;;) {
        lines.clear();
        current.clear();
        for (;; ++total) {
            const char c =
                line.read(1, total == 0 ? "without answering (is it an HTTP proxy?)" : "in the middle of its answer")[0];
            if (total == 0 && (c == '\x05' || c == '\x04' || c == '\0')) {
                throw ssh::Error(Failure::fatal, std::format("{} isn't an HTTP proxy (it answers like a SOCKS proxy: use "
                                                             "socks5://)",
                                                             line.where()));
            }
            if (total >= most_bytes) throw ssh::Error(Failure::fatal, too_long);
            if (c != '\n') {
                current += c;
                if (lines.empty() && current.size() == 5 && current != "HTTP/")
                    throw ssh::Error(Failure::fatal, std::format("{} isn't an HTTP proxy", line.where()));
                continue;
            }
            if (current.ends_with('\r')) current.pop_back();
            if (current.empty() && !lines.empty()) break;
            if (!current.empty()) lines.push_back(std::move(current));
            current.clear();
            if (lines.size() + lines_seen > most_lines) throw ssh::Error(Failure::fatal, too_long);
        }
        ++total;
        lines_seen += lines.size();

        // "HTTP/1.1 200 Connection established" (HTTP/1.0 too).
        status = lines.front();
        code = 0;
        const bool http1 = status.size() >= 12 && status.starts_with("HTTP/1.") && status[8] == ' ' &&
                           std::from_chars(status.data() + 9, status.data() + 12, code).ptr == status.data() + 12 &&
                           (status.size() == 12 || status[12] == ' ');
        if (!http1 || code < 100) throw ssh::Error(Failure::fatal, std::format("{} isn't an HTTP proxy", line.where()));
        if (code >= 200 || code == 101) break;
    }
    if (code >= 200 && code < 300) return; // the tunnel starts right after the empty line
    std::vector<std::string> schemes;
    for (std::size_t i = 1; i < lines.size(); ++i) {
        const std::string_view header = lines[i];
        const auto colon = header.find(':');
        if (colon == std::string_view::npos) continue;
        if (same_ignoring_case(util::trim(header.substr(0, colon)), "Proxy-Authenticate"))
            schemes_of(header.substr(colon + 1), schemes);
    }
    http_refusal(code, shown(status.substr(std::min<std::size_t>(13, status.size()))), schemes, login, host, port);
}

} // namespace grab::proxy
