#pragma once

// Questions of `grab server add` and `edit` that don't need a console to be tried: the console's
// reading and printing stay in server_cli.cpp, which hands them in. The proxy (PROXIES.md
// decision 5): asked as the window's form asks it, directly or through a SOCKS5 or an HTTP proxy,
// its address, its user and, with a user, its password, never shown.

#include "proxy.hpp"
#include "server_ops.hpp"
#include "servers.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace grab::server_prompt {

// How questions are put. `ask` shows the prompt, with `def` in brackets when there is one, and
// gives the answer trimmed (Enter: `def`); `secret` the same without echo, Enter an empty
// answer; nullopt from either when the input ran out. `say` prints a line between them: why an
// answer can't be taken, before the question comes again.
struct Console {
    std::function<std::optional<std::string>(std::string_view prompt, std::string_view def)> ask;
    std::function<std::optional<std::string>(std::string_view prompt)> secret;
    std::function<void(std::string_view line)> say;
};

// The proxy server `s` (its name and host) is reached through, asked with `offer` as the
// defaults (server_ops::edit_proxy for an edit; directly for a new server): "Connect  1) directly
// 2) through a SOCKS5 proxy  3) through an HTTP proxy", then the proxy's host:port, its user name
// (Enter keeps the offered one; "-" is none) and, with a user, its password, not shown:
// Enter keeps the saved one while the kind, address and user are the offered ones and one is
// saved, else Enter is none. Nothing is offered when `offer` has no proxy (its `why` is said
// first): one has to be chosen. Each answer is checked as grab.conf's `proxy` line is
// (server_conf::check_proxy), a SOCKS5 password for its 255 bytes, and a host with a zone can't
// go through a proxy. Returns the proxy with its password as typed (empty: none, or the saved
// one as above: NewServer::proxy), or nullopt when the input ran out.
[[nodiscard]] std::optional<proxy::Proxy> ask_proxy(const Console& io, const server_ops::ProxyOffer& offer,
                                                    const servers::NewServer& s);

} // namespace grab::server_prompt
