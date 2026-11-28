#pragma once

// One SSH server on 127.0.0.1 for one connection (test_ssh.cpp, test_proxy.cpp), and the KEXINIT
// packet it sends.

#ifdef _WIN32
#include <doctest/doctest.h>

#include <winsock2.h> // IWYU pragma: keep (the socket API)

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace grab::test {

// What a OneShotServer does with its one connection: sends its banner and `kexinit`, hangs up at
// once, reads the client's banner, sends its own and closes cleanly (a FIN, not a reset), or says
// nothing at all and never closes (a frozen server: 60 s at most, or until it is destroyed).
enum class Shot { talk, hang_up, banner_then_close, silent };

class OneShotServer {
public:
    explicit OneShotServer(std::string kexinit, Shot shot = Shot::talk) {
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
        listening_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in at{};
        at.sin_family = AF_INET;
        at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int length = sizeof(at);
        REQUIRE(::bind(listening_, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) == 0);
        REQUIRE(::listen(listening_, 1) == 0);
        REQUIRE(::getsockname(listening_, reinterpret_cast<sockaddr*>(&at), &length) == 0);
        port = ntohs(at.sin_port);
        thread_ = std::jthread([this, kexinit = std::move(kexinit), shot] {
            const SOCKET c = ::accept(listening_, nullptr, nullptr);
            if (c == ~SOCKET{0}) return; // INVALID_SOCKET: the listening socket was closed
            accepted_ = true;
            if (shot == Shot::silent) {
                // Not even a read: grab's FIN gets no answer, as from a frozen server.
                for (int i = 0; i < 600 && !done_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            } else if (shot != Shot::hang_up) {
                const DWORD wait = 5'000;
                ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&wait), sizeof(wait));
                if (shot == Shot::banner_then_close) {
                    char ch = 0;
                    while (::recv(c, &ch, 1, 0) == 1 && ch != '\n') {}
                }
                const std::string out = "SSH-2.0-FakeServer\r\n" + kexinit;
                ::send(c, out.data(), static_cast<int>(out.size()), 0);
                if (shot == Shot::banner_then_close) ::shutdown(c, SD_SEND);
                std::array<char, 4096> in{};
                while (::recv(c, in.data(), static_cast<int>(in.size()), 0) > 0) {}
            }
            closesocket(c);
        });
    }
    ~OneShotServer() { // accept() returns, then the thread is joined
        done_ = true;
        closesocket(listening_);
    }
    OneShotServer(const OneShotServer&) = delete;
    OneShotServer& operator=(const OneShotServer&) = delete;
    int port = 0;
    // Whether its one connection came (fail-closed checks: a proxy that failed sent nothing here).
    [[nodiscard]] bool accepted() const { return accepted_; }

private:
    SOCKET listening_ = INVALID_SOCKET;
    std::atomic<bool> done_{false};
    std::atomic<bool> accepted_{false};
    std::jthread thread_;
};

// An SSH binary packet holding a KEXINIT that offers only these algorithms.
inline std::string kexinit_packet(std::string_view kex, std::string_view host_key, std::string_view cipher,
                                  std::string_view mac) {
    auto u32 = [](std::string& out, std::uint32_t v) {
        for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>((v >> shift) & 0xFF));
    };
    std::string payload(1, '\x14'); // SSH_MSG_KEXINIT
    payload.append(16, 'c');        // cookie
    for (const std::string_view list : {kex, host_key, cipher, cipher, mac, mac, std::string_view("none"),
                                        std::string_view("none"), std::string_view(), std::string_view()}) {
        u32(payload, static_cast<std::uint32_t>(list.size()));
        payload += list;
    }
    payload.push_back('\0'); // first_kex_packet_follows
    u32(payload, 0);         // reserved
    std::size_t padding = 8 - (5 + payload.size()) % 8;
    if (padding < 4) padding += 8;
    std::string packet;
    u32(packet, static_cast<std::uint32_t>(1 + payload.size() + padding));
    packet.push_back(static_cast<char>(padding));
    return packet + payload + std::string(padding, '\0');
}

} // namespace grab::test
#endif
