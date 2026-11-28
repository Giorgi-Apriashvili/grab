#pragma once

// A proxy between grab and a server (PROXIES.md): SOCKS5 (RFC 1928, with RFC 1929's user name and
// password) or HTTP CONNECT (with or without a Basic login). ssh's connect_tcp dials the proxy
// instead of the server and runs its handshake on that socket; libssh2 then gets the tunnel as it
// would get a direct connection. The server's name always goes to the proxy, which looks it up:
// grab never resolves a proxied server's name. A proxy that fails fails the connection: nothing
// is ever tried directly.

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace grab::proxy {

enum class Kind { none, socks5, http };

struct Proxy {
    Kind kind = Kind::none; // none: directly
    std::string host;       // a name or an address (IPv6 without brackets)
    int port = 0;
    std::string user;     // empty: no login
    std::string password; // memory only, like Target::password; sent only when a user is set
    bool operator==(const Proxy&) const = default;
};

// grab.conf's `proxy` value (and the form's): socks5://[user@]host:port (socks5h:// means the
// same: the proxy always looks the server's name up), or http://[user@]host:port, the scheme in
// any case, an IPv6 address in brackets, the user percent-encoded, a trailing '/' allowed. The
// port is required. A password in it is refused (it belongs in grab's store; `server` names the
// server in that reason: "Edit Box"), and so are the kinds grab can't use yet (socks4://,
// https://). The error is the reason alone; the caller adds "[Box] proxy: ".
[[nodiscard]] std::expected<Proxy, std::string> parse(std::string_view text, std::string_view server = "the server");
// rclone's sftp `socks_proxy`, split as rclone 1.75.1's lib/proxy splits it: user:pass@host:port,
// user@host:port or host:port, at the first '@' and then the first ':', nothing percent-decoded.
[[nodiscard]] std::expected<Proxy, std::string> parse_rclone_socks(std::string_view text);
// rclone's sftp `http_proxy`: an http:// URL whose user and password are percent-decoded; without
// a port 80, as rclone dialled it. https:// (TLS to the proxy) is refused: grab can't do it yet.
[[nodiscard]] std::expected<Proxy, std::string> parse_rclone_http(std::string_view text);
// A proxy's address as `grab server add` and `edit` ask for it: host:port, an IPv6 address in
// brackets ([::1]:1080), the port required; the kind and the user are asked on their own. Kind
// none, no user.
[[nodiscard]] std::expected<Proxy, std::string> parse_address(std::string_view text);

// The proxy as grab.conf writes it, without the password: "socks5://alice@127.0.0.1:9050",
// "http://[::1]:3128" (parse reads it back); empty for none.
[[nodiscard]] std::string text(const Proxy& proxy);
// The proxy in rclone's form, its password included, for the copy grab keeps in its store for
// older grabs (PROXIES.md decision 6): a SOCKS5 one as `socks_proxy` (user:password@host:port,
// user@host:port or host:port, nothing encoded, as rclone 1.75.1 splits it), an HTTP one as
// `http_proxy` (http://user:password@host:port, user and password percent-encoded).
// parse_rclone_socks / parse_rclone_http read it back (a SOCKS5 user or password holding '@' or
// ':' only as rclone would split it); empty for none. A secret: never shown.
[[nodiscard]] std::string rclone_text(const Proxy& proxy);
// A short description for messages, without the password: "the SOCKS5 proxy alice@127.0.0.1:9050".
[[nodiscard]] std::string said(const Proxy& proxy);
// host:port for messages, an IPv6 address in brackets: "127.0.0.1:9050", "[::1]:1080".
[[nodiscard]] std::string address(std::string_view host, int port);
// "SOCKS5" or "HTTP" (empty for none).
[[nodiscard]] std::string_view kind_name(Kind kind);

// Why asking `proxy` for host:port can't be done, checked before anything is sent: a server name
// a proxy can't take (empty, longer than 255 bytes, control characters or spaces, not ASCII: its
// xn-- form is), an IPv6 address with a zone, a user name with control characters or spaces (or a
// ':' for an HTTP proxy's Basic login), a SOCKS5 user or password longer than 255 bytes. Never
// quotes the password. Nothing when it can be asked.
[[nodiscard]] std::optional<std::string> check_request(const Proxy& proxy, std::string_view host, int port);

// How long a handshake may take: until `limit` after `started` (connect_timeout from the moment
// the proxy was dialled; 0: no limit), and `cancelled` (the program closing), looked at every
// 250 ms.
struct Wait {
    std::chrono::steady_clock::time_point started;
    std::chrono::milliseconds limit{0};
    std::function<bool()> cancelled;
};

// The handshake on `socket` (connected to the proxy, non-blocking): afterwards it is a tunnel to
// host:port, and not one byte of what the server sent through it was read (its SSH banner can
// come in the proxy's reply's packet). Throws ssh::Error: its kind as PROXIES.md 3.4 has it, the
// text plain (3.8) and never holding the password; "cancelled" (fatal) when `wait.cancelled`
// said so. A proxy turning grab away (SOCKS 02, HTTP 403/405) is `refused` with
// Error::proxied fatal, so is a proxy closing the connection before it answers (proxied other).
void socks5_connect(std::uintptr_t socket, const Proxy& proxy, std::string_view host, int port, const Wait& wait);
void http_connect(std::uintptr_t socket, const Proxy& proxy, std::string_view host, int port, const Wait& wait);

// A time as the messages say it: "2 min", "30 s", "1.5 s", "500 ms".
[[nodiscard]] std::string say_time(std::chrono::milliseconds t);

} // namespace grab::proxy
