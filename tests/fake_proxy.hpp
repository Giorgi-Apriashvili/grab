#pragma once

// Fake proxies for the proxy tests (PROXIES.md section 5): a SOCKS5 or HTTP CONNECT server on
// 127.0.0.1 (or ::1) on a port of its own, a thread for each connection, all doing what the
// script says: the login it wants, the answer it gives (a refusal, a wrong one, or none at all),
// whole or one byte at a time, and after a success a relay to a port on 127.0.0.1 (a
// OneShotServer, rclone serve sftp), the bytes `serve` in the same send as the answer (a server's
// banner), or the connection closed. What each connection sent is kept, as it arrives.

#ifdef _WIN32
#include "proxy.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <winsock2.h> // IWYU pragma: keep (the socket API)
#include <ws2tcpip.h> // IWYU pragma: keep (in6addr_loopback)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace grab::test {

struct ProxyScript {
    proxy::Kind kind = proxy::Kind::socks5;
    // The login it wants (no user: none). SOCKS5: the method it picks (else 02 when it wants a
    // login and it was offered, 00 when it wants none and that was offered, FF otherwise) and the
    // login's status (else 00 for the right user and password, 01 for any other).
    std::string user;
    std::string password;
    std::optional<unsigned char> method;
    std::optional<unsigned char> status;
    // SOCKS5: the CONNECT reply's code and what follows it (BND.ATYP, BND.ADDR, BND.PORT).
    unsigned char rep = 0x00;
    std::string bound = std::string("\x01\0\0\0\0\0\0", 7);
    // HTTP: the answer to a CONNECT it takes (its last status a 2xx: a success), and the challenge of its 407
    // to one without the right login.
    std::string answer = "HTTP/1.1 200 Connection established\r\n\r\n";
    std::string challenge = "Proxy-Authenticate: Basic realm=\"fake\"\r\n";
    // These bytes after the client's first ones instead of any answer of its kind (a wrong one).
    std::optional<std::string> wrong;
    bool silent = false;   // takes the connection and says nothing, ever
    bool hang_up = false;  // closes it at once
    bool one_byte = false; // its answers one byte at a time
    // After a success: relayed to 127.0.0.1:relay_port, else `serve` goes out in the same send as
    // the answer, and `close_after` closes the connection right after it.
    int relay_port = 0;
    std::string serve;
    bool close_after = false;
};

// What one connection sent.
struct ProxyRequest {
    std::string greeting; // SOCKS5: VER NMETHODS METHODS
    std::string login;    // SOCKS5: RFC 1929's request, when there was one
    std::string connect;  // SOCKS5: the CONNECT request
    std::string header;   // HTTP: the request to its empty line
};

class FakeProxy {
public:
    explicit FakeProxy(ProxyScript script, bool v6 = false) : script_(std::move(script)), v6_(v6) {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
        listening_ = ::socket(v6 ? AF_INET6 : AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (v6) {
            sockaddr_in6 at{};
            at.sin6_family = AF_INET6;
            at.sin6_addr = in6addr_loopback;
            int length = sizeof(at);
            REQUIRE(::bind(listening_, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) == 0);
            REQUIRE(::getsockname(listening_, reinterpret_cast<sockaddr*>(&at), &length) == 0);
            port = ntohs(at.sin6_port);
        } else {
            sockaddr_in at{};
            at.sin_family = AF_INET;
            at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            int length = sizeof(at);
            REQUIRE(::bind(listening_, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) == 0);
            REQUIRE(::getsockname(listening_, reinterpret_cast<sockaddr*>(&at), &length) == 0);
            port = ntohs(at.sin_port);
        }
        REQUIRE(::listen(listening_, 16) == 0);
        accepting_ = std::jthread([this] { accept_all(); });
    }
    ~FakeProxy() {
        done_ = true;
        closesocket(listening_);
        accepting_.join();
        connections_.clear(); // each ends within 100 ms of done_
    }
    FakeProxy(const FakeProxy&) = delete;
    FakeProxy& operator=(const FakeProxy&) = delete;

    int port = 0;
    [[nodiscard]] int connections() const { return accepted_; }
    [[nodiscard]] std::vector<ProxyRequest> requests() const {
        std::scoped_lock lock(mutex_);
        return requests_;
    }
    // The proxy as a Target takes it.
    [[nodiscard]] proxy::Proxy proxy(std::string user = {}, std::string password = {}) const {
        return proxy::Proxy{.kind = script_.kind,
                            .host = v6_ ? "::1" : "127.0.0.1",
                            .port = port,
                            .user = std::move(user),
                            .password = std::move(password)};
    }

private:
    using Clock = std::chrono::steady_clock;

    void accept_all() {
        for (;;) {
            const SOCKET c = ::accept(listening_, nullptr, nullptr);
            if (c == ~SOCKET{0}) return; // INVALID_SOCKET: the listening socket was closed
            if (done_) {
                closesocket(c);
                return;
            }
            ++accepted_;
            std::size_t index = 0;
            {
                std::scoped_lock lock(mutex_);
                index = requests_.size();
                requests_.emplace_back();
            }
            connections_.emplace_back([this, c, index] {
                serve_one(c, index);
                closesocket(c);
            });
        }
    }

    // What connection `index` sent, kept before the answer to it goes out.
    void note(std::size_t index, const std::function<void(ProxyRequest&)>& change) {
        std::scoped_lock lock(mutex_);
        change(requests_[index]);
    }

    // Whether `s` has something to read within 100 ms.
    static bool readable(SOCKET s) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(s, &r);
        timeval tv{.tv_sec = 0, .tv_usec = 100'000};
        return select(0, &r, nullptr, nullptr, &tv) > 0;
    }

    // Exactly `n` more bytes onto `out`; false when the client closed first (or 10 s passed).
    bool read_exact(SOCKET s, std::size_t n, std::string& out) const {
        const auto until = Clock::now() + std::chrono::seconds(10);
        std::array<char, 4096> buffer{};
        while (n > 0) {
            if (done_ || Clock::now() > until) return false;
            if (!readable(s)) continue;
            const int r = ::recv(s, buffer.data(), static_cast<int>(std::min(n, buffer.size())), 0);
            if (r <= 0) return false;
            out.append(buffer.data(), static_cast<std::size_t>(r));
            n -= static_cast<std::size_t>(r);
        }
        return true;
    }

    void send_out(SOCKET s, std::string_view bytes) const {
        if (!script_.one_byte) {
            ::send(s, bytes.data(), static_cast<int>(bytes.size()), 0);
            return;
        }
        for (const char c : bytes) {
            ::send(s, &c, 1, 0);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Reads until the client closes (10 s at most).
    void drain(SOCKET s) const {
        std::string ignored;
        while (read_exact(s, 1, ignored)) ignored.clear();
    }

    static SOCKET dial(int to) {
        const SOCKET up = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in at{};
        at.sin_family = AF_INET;
        at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        at.sin_port = htons(static_cast<u_short>(to));
        if (::connect(up, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) != 0) {
            closesocket(up);
            return INVALID_SOCKET;
        }
        return up;
    }

    // Both ways until either side closes.
    void relay(SOCKET a, SOCKET b) const {
        std::array<char, 16384> buffer{};
        while (!done_) {
            fd_set r;
            FD_ZERO(&r);
            FD_SET(a, &r);
            FD_SET(b, &r);
            timeval tv{.tv_sec = 0, .tv_usec = 100'000};
            if (select(0, &r, nullptr, nullptr, &tv) <= 0) continue;
            for (const auto [from, to] : {std::pair{a, b}, std::pair{b, a}}) {
                if (!FD_ISSET(from, &r)) continue;
                const int n = ::recv(from, buffer.data(), static_cast<int>(buffer.size()), 0);
                if (n <= 0) return;
                for (int sent = 0; sent < n;) {
                    const int m = ::send(to, buffer.data() + sent, n - sent, 0);
                    if (m <= 0) return;
                    sent += m;
                }
            }
        }
    }

    // The success answer, and what follows it.
    void succeed(SOCKET c, const std::string& answer) const {
        if (script_.relay_port != 0) {
            const SOCKET up = dial(script_.relay_port);
            if (up == ~SOCKET{0}) return; // INVALID_SOCKET
            send_out(c, answer);
            relay(c, up);
            closesocket(up);
            return;
        }
        // In the same send as the answer; one byte at a time, the answer's bytes only.
        if (script_.one_byte) {
            send_out(c, answer);
            ::send(c, script_.serve.data(), static_cast<int>(script_.serve.size()), 0);
        } else {
            send_out(c, answer + script_.serve);
        }
        if (!script_.close_after) drain(c);
    }

    void serve_one(SOCKET c, std::size_t index) {
        const int one = 1;
        ::setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
        if (script_.hang_up) return;
        if (script_.silent) {
            for (int i = 0; i < 600 && !done_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return;
        }
        if (script_.wrong) {
            std::string first;
            if (!read_exact(c, 1, first)) return;
            send_out(c, *script_.wrong);
            drain(c);
            return;
        }
        if (script_.kind == proxy::Kind::socks5) socks5(c, index);
        else http(c, index);
    }

    void socks5(SOCKET c, std::size_t index) {
        std::string greeting;
        if (!read_exact(c, 2, greeting) || !read_exact(c, static_cast<unsigned char>(greeting[1]), greeting)) return;
        note(index, [&](ProxyRequest& r) { r.greeting = greeting; });
        const std::string_view offered = std::string_view(greeting).substr(2);
        const unsigned char method = script_.method.value_or(
            script_.user.empty() ? (offered.contains('\0') ? 0x00 : 0xFF) : (offered.contains('\x02') ? 0x02 : 0xFF));
        send_out(c, std::string{'\x05', static_cast<char>(method)});
        if (method == 0xFF) {
            drain(c);
            return;
        }
        if (method == 0x02) {
            std::string login;
            if (!read_exact(c, 2, login)) return;
            const std::size_t ulen = static_cast<unsigned char>(login[1]);
            if (!read_exact(c, ulen + 1, login)) return;
            const std::size_t plen = static_cast<unsigned char>(login.back());
            if (!read_exact(c, plen, login)) return;
            note(index, [&](ProxyRequest& r) { r.login = login; });
            const bool right = login.substr(2, ulen) == script_.user && login.substr(3 + ulen) == script_.password;
            const unsigned char status = script_.status.value_or(right ? 0x00 : 0x01);
            send_out(c, std::string{'\x01', static_cast<char>(status)});
            if (status != 0) {
                drain(c);
                return;
            }
        }
        std::string request;
        if (!read_exact(c, 4, request)) return;
        const char atyp = request[3];
        const bool read_all =
            atyp == '\x01'   ? read_exact(c, 4 + 2, request)
            : atyp == '\x04' ? read_exact(c, 16 + 2, request)
                             : read_exact(c, 1, request) && read_exact(c, static_cast<unsigned char>(request.back()) + 2u, request);
        if (!read_all) return;
        note(index, [&](ProxyRequest& r) { r.connect = request; });
        const std::string answer = std::string{'\x05', static_cast<char>(script_.rep), '\0'} + script_.bound;
        if (script_.rep != 0) {
            send_out(c, answer);
            drain(c);
            return;
        }
        succeed(c, answer);
    }

    void http(SOCKET c, std::size_t index) {
        std::string header;
        while (!header.ends_with("\r\n\r\n")) {
            if (header.size() > 65536 || !read_exact(c, 1, header)) return;
        }
        note(index, [&](ProxyRequest& r) { r.header = header; });
        if (!script_.user.empty()) {
            const std::string want =
                "Proxy-Authorization: Basic " + util::base64_encode(script_.user + ":" + script_.password) + "\r\n";
            if (!header.contains(want)) {
                send_out(c, "HTTP/1.1 407 Proxy Authentication Required\r\n" + script_.challenge + "\r\n");
                drain(c);
                return;
            }
        }
        // The last status line decides: interim 1xx answers may come before it.
        const auto last = script_.answer.rfind("HTTP/1.");
        const bool success = last != std::string::npos && script_.answer.size() > last + 9 && script_.answer[last + 9] == '2';
        if (!success) {
            send_out(c, script_.answer);
            drain(c);
            return;
        }
        succeed(c, script_.answer);
    }

    ProxyScript script_;
    bool v6_ = false;
    SOCKET listening_ = INVALID_SOCKET;
    std::atomic<bool> done_{false};
    std::atomic<int> accepted_{0};
    mutable std::mutex mutex_;
    std::vector<ProxyRequest> requests_;
    std::vector<std::jthread> connections_; // only the accepting thread adds, until it is joined
    std::jthread accepting_;
};

} // namespace grab::test
#endif
