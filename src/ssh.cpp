#include "ssh.hpp"

#include "util.hpp"

#ifdef _WIN32
#include <winsock2.h> // IWYU pragma: keep
#include <ws2tcpip.h> // IWYU pragma: keep
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <libssh2.h>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <vector>

namespace grab::ssh {

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t no_socket = INVALID_SOCKET;
void close_socket(socket_t s) { closesocket(s); }
int last_socket_error() { return WSAGetLastError(); }
#else
using socket_t = int;
constexpr socket_t no_socket = -1;
void close_socket(socket_t s) { ::close(s); }
int last_socket_error() { return errno; }
#endif

void init_once() {
    static std::once_flag once;
    std::call_once(once, [] {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        libssh2_init(0);
    });
}

void set_blocking(socket_t s, bool blocking) {
#ifdef _WIN32
    u_long nonblocking = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &nonblocking);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK));
#endif
}

// The addresses of `name` (port 0 when empty), freed with them.
struct Addresses {
    addrinfo* first = nullptr;
    Addresses() = default;
    Addresses(const Addresses&) = delete;
    Addresses& operator=(const Addresses&) = delete;
    ~Addresses() {
        if (first) freeaddrinfo(first);
    }
    bool resolve(const std::string& name, int port) {
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = port == 0 ? AI_PASSIVE : 0;
        return getaddrinfo(name.c_str(), std::to_string(port).c_str(), &hints, &first) == 0 && first;
    }
};

// A time in ms as Winsock's select and libssh2 take it (32 bits): one longer than that is no limit
// (0) rather than one wrapped round to something short (parse_duration already maps these).
std::int32_t ms_or_none(std::chrono::milliseconds t) {
    const auto ms = t.count();
    return ms > 0 && ms <= std::numeric_limits<std::int32_t>::max() ? static_cast<std::int32_t>(ms) : 0;
}

// A TCP connection to host:port within `connect_timeout`, from the `bind` address when one is set
// (an address of the same family as the server's).
socket_t connect_tcp(const Target& t) {
    Addresses found;
    if (!found.resolve(t.host, t.port))
        throw Error(budget::Failure::other, std::format("can't find the server {}", t.host));
    Addresses local;
    if (!t.bind.empty() && !local.resolve(t.bind, 0))
        throw Error(budget::Failure::fatal, std::format("can't find the address {} to connect from (--bind)", t.bind));
    auto from_for = [&](const addrinfo* a) -> const addrinfo* {
        for (const addrinfo* l = local.first; l; l = l->ai_next) {
            if (l->ai_family == a->ai_family) return l;
        }
        return nullptr;
    };
    if (local.first) {
        bool any = false;
        for (const addrinfo* a = found.first; a && !any; a = a->ai_next) any = from_for(a) != nullptr;
        if (!any)
            throw Error(budget::Failure::fatal,
                        std::format("{} (--bind) and {} have no address of the same kind (IPv4, IPv6)", t.bind, t.host));
    }
    std::string why = "no address";
    socket_t s = no_socket;
    for (addrinfo* a = found.first; a && s == no_socket; a = a->ai_next) {
        const addrinfo* from = from_for(a);
        if (local.first && !from) continue;
        socket_t c = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (c == no_socket) continue;
        if (from && ::bind(c, from->ai_addr, static_cast<int>(from->ai_addrlen)) != 0) {
            const int e = last_socket_error();
            close_socket(c);
            throw Error(budget::Failure::fatal, std::format("can't connect from {} (--bind): error {}", t.bind, e));
        }
        set_blocking(c, false);
        int rc = ::connect(c, a->ai_addr, static_cast<int>(a->ai_addrlen));
        bool ok = rc == 0;
        if (!ok) {
            int e = last_socket_error();
#ifdef _WIN32
            bool in_progress = e == WSAEWOULDBLOCK;
#else
            bool in_progress = e == EINPROGRESS;
#endif
            if (in_progress) {
                fd_set w;
                fd_set x;
                FD_ZERO(&w);
                FD_ZERO(&x);
                FD_SET(c, &w);
                FD_SET(c, &x);
                const std::int32_t ms = ms_or_none(t.connect_timeout);
                timeval tv{.tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000};
                // No limit: as long as the system tries. A refusal comes as an exception (Windows).
                if (select(static_cast<int>(c + 1), nullptr, &w, &x, ms > 0 ? &tv : nullptr) > 0) {
                    int err = 0;
                    socklen_t len = sizeof err;
                    getsockopt(c, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
                    ok = err == 0 && FD_ISSET(c, &w);
                    why = ok ? "" : std::format("connection failed ({})", err);
                } else {
                    why = "no answer";
                }
            } else {
                why = std::format("connection failed ({})", e);
            }
        }
        if (ok) {
            set_blocking(c, true);
            s = c;
        } else {
            close_socket(c);
        }
    }
    if (s == no_socket) throw Error(budget::Failure::other, std::format("can't reach {}:{}: {}", t.host, t.port, why));
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof one);
    return s;
}

std::string last_error(LIBSSH2_SESSION* s) {
    char* msg = nullptr;
    int code = libssh2_session_last_error(s, &msg, nullptr, 0);
    return msg && *msg ? std::string(msg) : std::format("libssh2 error {}", code);
}

// Waits at most `most` (at most 1 s) for the socket to bring something, or to take what libssh2
// is blocked on sending (non-blocking mode).
void wait_socket(socket_t s, LIBSSH2_SESSION* session, std::chrono::milliseconds most) {
    fd_set r;
    fd_set w;
    FD_ZERO(&r);
    FD_ZERO(&w);
    FD_SET(s, &r);
    if (libssh2_session_block_directions(session) & LIBSSH2_SESSION_BLOCK_OUTBOUND) FD_SET(s, &w);
    auto ms = std::clamp<long long>(most.count(), 0, 1000);
    timeval tv{.tv_sec = static_cast<long>(ms / 1000), .tv_usec = static_cast<long>((ms % 1000) * 1000)};
    select(static_cast<int>(s + 1), &r, &w, nullptr, &tv);
}

// Ends the TCP connection under libssh2, so whatever it still sends or awaits fails at once.
void cut(socket_t s) {
#ifdef _WIN32
    ::shutdown(s, SD_BOTH);
#else
    ::shutdown(s, SHUT_RDWR);
#endif
}

// An SSH channel as the byte stream an sftp::Client reads and writes.
class ChannelStream : public sftp::Channel {
public:
    ChannelStream(LIBSSH2_SESSION* session, LIBSSH2_CHANNEL* channel) : session_(session), channel_(channel) {}
    ~ChannelStream() override { libssh2_channel_free(channel_); }
    ChannelStream(const ChannelStream&) = delete;
    ChannelStream& operator=(const ChannelStream&) = delete;

    void write(std::string_view bytes) override {
        while (!bytes.empty()) {
            auto n = libssh2_channel_write(channel_, bytes.data(), bytes.size());
            if (n < 0) throw sftp::Broken("sending to the server failed: " + last_error(session_));
            bytes.remove_prefix(static_cast<std::size_t>(n));
        }
    }
    std::size_t read(std::span<char> buffer) override {
        auto n = libssh2_channel_read(channel_, buffer.data(), buffer.size());
        if (n < 0) throw sftp::Broken("reading from the server failed: " + last_error(session_));
        if (n == 0) throw sftp::Broken("the server closed the SFTP channel");
        return static_cast<std::size_t>(n);
    }

private:
    LIBSSH2_SESSION* session_;
    LIBSSH2_CHANNEL* channel_;
};

// The receive window offered per channel: more than the 64 READs of 255 KiB a connection keeps in
// flight (sftp.hpp), so the window never holds the server back.
constexpr unsigned window_size = 32 * 1024 * 1024;
// A command's: plenty for find's output, and little still on its way when a command is dropped
// (the drop reads it while it waits for the server's close).
constexpr unsigned command_window = 2 * 1024 * 1024;

// A command's channel on a libssh2 session, which is non-blocking while it lives.
class SshCommand final : public CommandChannel {
public:
    SshCommand(LIBSSH2_SESSION* session, socket_t socket, const std::atomic<std::uint64_t>& received, bool& dropped)
        : session_(session), socket_(socket), received_(received), dropped_(dropped) {
        libssh2_session_set_blocking(session_, 0);
        libssh2_keepalive_config(session_, 1, 1); // with want_reply; 1 is libssh2's least, 2 s
    }
    ~SshCommand() override {
        if (channel_) drop(true); // left by an exception: the connection is done for
        // Keepalives off again: in blocking mode libssh2's waits would send one every 2 s and give
        // up after a second or two of silence instead of the session's timeout.
        libssh2_keepalive_config(session_, 0, 0);
        libssh2_session_set_blocking(session_, 1);
    }
    SshCommand(const SshCommand&) = delete;
    SshCommand& operator=(const SshCommand&) = delete;

    std::optional<bool> start(std::string_view command) override {
        if (!channel_) {
            channel_ = libssh2_channel_open_ex(session_, "session", 7, command_window, LIBSSH2_CHANNEL_PACKET_DEFAULT,
                                               nullptr, 0);
            if (!channel_) {
                int code = libssh2_session_last_errno(session_);
                if (code == LIBSSH2_ERROR_EAGAIN) return std::nullopt;
                if (code == LIBSSH2_ERROR_CHANNEL_FAILURE) { // the server refused the channel (MaxSessions 1)
                    refusal_ = last_error(session_);
                    return false;
                }
                fail("can't open a channel");
            }
        }
        int rc = libssh2_channel_process_startup(channel_, "exec", 4, command.data(),
                                                 static_cast<unsigned>(command.size()));
        if (rc == LIBSSH2_ERROR_EAGAIN) return std::nullopt;
        if (rc == LIBSSH2_ERROR_CHANNEL_REQUEST_DENIED) return false;
        if (rc != 0) fail("the server didn't run the command");
        ran_ = true;
        return true;
    }
    bool send_eof() override {
        int rc = libssh2_channel_send_eof(channel_);
        if (rc == LIBSSH2_ERROR_EAGAIN) return false;
        if (rc != 0) fail("ending the command's input failed");
        return true;
    }
    std::size_t read(bool err, std::span<char> buffer) override {
        auto n = libssh2_channel_read_ex(channel_, err ? SSH_EXTENDED_DATA_STDERR : 0, buffer.data(), buffer.size());
        if (n == LIBSSH2_ERROR_EAGAIN) return 0;
        if (n < 0) fail("reading the command's output failed");
        return static_cast<std::size_t>(n);
    }
    bool ended() override { return libssh2_channel_eof(channel_) != 0; }
    bool close() override {
        if (!channel_) return true; // refused at the open: there is nothing to close
        // Sends our close (0 at once on later calls), but returns after any one packet, which may
        // be a keepalive's answer. The server's close comes after the exit status (OpenSSH), so it
        // is awaited too; not after a refused exec request, which sent no EOF (wait_closed wants one).
        int rc = libssh2_channel_close(channel_);
        if (rc == LIBSSH2_ERROR_EAGAIN) return false;
        if (rc != 0) fail("closing the command's channel failed");
        if (ran_) {
            rc = libssh2_channel_wait_closed(channel_);
            if (rc == LIBSSH2_ERROR_EAGAIN) return false;
            if (rc != 0) fail("closing the command's channel failed");
        }
        char* name = nullptr;
        std::size_t len = 0;
        libssh2_channel_get_exit_signal(channel_, &name, &len, nullptr, nullptr, nullptr, nullptr);
        if (name) {
            signal_.assign(name, len);
            libssh2_free(session_, name);
        }
        // libssh2 says 0 when no status came: -1 then.
        status_ = signal_.empty() && libssh2_channel_has_exit_status(channel_) != 0
                      ? libssh2_channel_get_exit_status(channel_)
                      : -1;
        libssh2_channel_free(channel_); // closed both ways, or refused (a late close is ignored)
        channel_ = nullptr;
        return true;
    }
    int exit_status() override { return status_; }
    std::string exit_signal() override { return signal_; }
    std::string refusal() override { return refusal_; }
    void terminate() override {
        if (!channel_) return;
        auto until = Clock::now() + std::chrono::seconds(1);
        while (libssh2_channel_signal_ex(channel_, "TERM", 4) == LIBSSH2_ERROR_EAGAIN && Clock::now() < until)
            wait(std::chrono::milliseconds(100));
    }
    void drop(bool dead) override {
        dropped_ = true;
        if (dead) cut(socket_);
        if (!channel_) return; // still opening: it goes with the session
        // The server's close is awaited a little, reading what is still on its way; past that the
        // link is cut, so that the channel goes at once.
        auto until = Clock::now() + std::chrono::seconds(2);
        for (int after_cut = 0; libssh2_channel_free(channel_) == LIBSSH2_ERROR_EAGAIN && after_cut < 3;) {
            if (Clock::now() < until) {
                wait(std::chrono::milliseconds(100));
            } else {
                cut(socket_);
                ++after_cut;
            }
        }
        channel_ = nullptr; // freed, or else freed with the session
    }
    void keepalive() override {
        int next = 0;
        if (libssh2_keepalive_send(session_, &next) != 0) fail("sending a keepalive failed");
    }
    std::uint64_t received() override { return received_.load(std::memory_order_relaxed); }
    void wait(std::chrono::milliseconds most) override { wait_socket(socket_, session_, most); }

private:
    using Clock = std::chrono::steady_clock;
    // The link failed: the connection is dropped and cut, so that nothing waits on it.
    [[noreturn]] void fail(std::string_view what) {
        std::string why = std::format("{}: {}", what, last_error(session_));
        dropped_ = true;
        cut(socket_);
        throw Error(budget::Failure::other, why);
    }

    LIBSSH2_SESSION* session_;
    socket_t socket_;
    const std::atomic<std::uint64_t>& received_;
    bool& dropped_;
    LIBSSH2_CHANNEL* channel_ = nullptr;
    bool ran_ = false; // the server took the exec request
    int status_ = -1;
    std::string signal_;
    std::string refusal_;
};

} // namespace

std::string pinned_algorithms(std::string_view known_hosts_text, std::string_view host, int port) {
    std::string want = port == 22 ? std::string(host) : std::format("[{}]:{}", host, port);
    std::string out;
    std::istringstream lines{std::string(known_hosts_text)};
    std::string line;
    auto add = [&out](std::string_view algs) {
        if (out.find(algs) != std::string::npos) return;
        if (!out.empty()) out += ',';
        out += algs;
    };
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == '@') continue;
        std::istringstream fields(line);
        std::string hosts, type;
        fields >> hosts >> type;
        bool match = false;
        for (std::size_t b = 0; b <= hosts.size() && !match;) {
            std::size_t e = std::min(hosts.find(',', b), hosts.size());
            match = std::string_view(hosts).substr(b, e - b) == want;
            b = e + 1;
        }
        if (!match) continue;
        if (type == "ssh-ed25519" || type.starts_with("ecdsa-sha2-nistp")) add(type);
        else if (type == "ssh-rsa") add("rsa-sha2-512,rsa-sha2-256"); // the key, with SHA-2 signatures
    }
    return out;
}

// ---- host keys -------------------------------------------------------------------------------

std::string key_type(std::string_view blob) {
    if (blob.size() < 4) return {};
    std::size_t len = 0;
    for (int i = 0; i < 4; ++i) len = (len << 8) | static_cast<unsigned char>(blob[static_cast<std::size_t>(i)]);
    if (len == 0 || len > 64 || len > blob.size() - 4) return {};
    std::string type(blob.substr(4, len));
    const bool plain = std::ranges::all_of(type, [](char c) { return c > ' ' && c < 0x7F; });
    return plain ? type : std::string();
}

std::string fingerprint(std::string_view blob) {
    std::array<unsigned char, 32> digest{};
    unsigned int len = 0;
    EVP_Digest(blob.data(), blob.size(), digest.data(), &len, EVP_sha256(), nullptr);
    std::string hash = util::base64_encode(std::string_view(reinterpret_cast<const char*>(digest.data()), len));
    while (hash.ends_with('=')) hash.pop_back();
    return "SHA256:" + hash;
}

std::optional<HostKey> host_key(std::string_view blob) {
    std::string type = key_type(blob);
    if (type.empty()) return std::nullopt;
    return HostKey{.type = std::move(type), .key = util::base64_encode(blob), .fingerprint = fingerprint(blob)};
}

std::string known_host_line(std::string_view host, int port, const HostKey& key) {
    const std::string name = port == 22 ? std::string(host) : std::format("[{}]:{}", host, port);
    return std::format("{} {} {}", name, key.type, key.key);
}

namespace {

int knownhost_key_bit(std::string_view type) {
    if (type == "ssh-rsa") return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    if (type == "ecdsa-sha2-nistp256") return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
    if (type == "ecdsa-sha2-nistp384") return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
    if (type == "ecdsa-sha2-nistp521") return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
    if (type == "ssh-ed25519") return LIBSSH2_KNOWNHOST_KEY_ED25519;
    return 0;
}

// check_known_host on a session's known-hosts store (libssh2 needs a session for one).
KnownHost check_known(LIBSSH2_SESSION* session, std::string_view text, std::string_view host, int port,
                      std::string_view blob) {
    const int keybit = knownhost_key_bit(key_type(blob));
    if (keybit == 0) return KnownHost::unknown;
    LIBSSH2_KNOWNHOSTS* known = libssh2_knownhost_init(session);
    if (!known) throw Error(budget::Failure::other, "can't check the host key");
    for (std::size_t start = 0; start < text.size();) {
        std::size_t end = std::min(text.find('\n', start), text.size());
        std::string line(text.substr(start, end - start));
        start = end + 1;
        if (line.ends_with('\r')) line.pop_back();
        if (line.empty() || line.front() == '#') continue;
        // A line libssh2 can't read (a key type it doesn't know) holds nothing it could match.
        (void)libssh2_knownhost_readline(known, line.c_str(), line.size(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    }
    libssh2_knownhost* match = nullptr;
    const int check = libssh2_knownhost_checkp(known, std::string(host).c_str(), port, blob.data(), blob.size(),
                                               LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | keybit, &match);
    libssh2_knownhost_free(known);
    if (check == LIBSSH2_KNOWNHOST_CHECK_MATCH) return KnownHost::match;
    if (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) return KnownHost::mismatch;
    return KnownHost::unknown;
}

// One handshake that asks for the host key algorithms in `offer` and logs in to nothing: the blob
// of the key the server showed.
std::expected<std::string, ShakeError> read_host_key(const Target& t, std::string_view offer) {
    socket_t s = no_socket;
    try {
        s = connect_tcp(t);
    } catch (const Error& e) {
        return std::unexpected(ShakeError{.no_match = false, .message = e.what()});
    }
    LIBSSH2_SESSION* session = libssh2_session_init_ex(nullptr, nullptr, nullptr, nullptr);
    std::expected<std::string, ShakeError> result = std::unexpected(ShakeError{.message = "can't start an SSH session"});
    if (session) {
        libssh2_session_set_blocking(session, 1);
        libssh2_session_set_timeout(session, ms_or_none(t.timeout));
        libssh2_session_method_pref(session, LIBSSH2_METHOD_KEX, std::string(kex_algorithms).c_str());
        libssh2_session_method_pref(session, LIBSSH2_METHOD_CRYPT_CS, std::string(cipher_algorithms).c_str());
        libssh2_session_method_pref(session, LIBSSH2_METHOD_CRYPT_SC, std::string(cipher_algorithms).c_str());
        libssh2_session_method_pref(session, LIBSSH2_METHOD_MAC_CS, std::string(mac_algorithms).c_str());
        libssh2_session_method_pref(session, LIBSSH2_METHOD_MAC_SC, std::string(mac_algorithms).c_str());
        std::size_t len = 0;
        int type = 0;
        if (libssh2_session_method_pref(session, LIBSSH2_METHOD_HOSTKEY, std::string(offer).c_str()) != 0) {
            result = std::unexpected(ShakeError{.message = last_error(session)});
        } else if (int rc = libssh2_session_handshake(session, s); rc != 0) {
            // No algorithm in common; with the same key exchanges and ciphers as the first
            // handshake, that is the host key types asked for.
            const bool no_match = rc == LIBSSH2_ERROR_KEX_FAILURE || rc == LIBSSH2_ERROR_METHOD_NOT_SUPPORTED;
            result = std::unexpected(ShakeError{.no_match = no_match, .message = last_error(session)});
        } else if (const char* key = libssh2_session_hostkey(session, &len, &type); key && len > 0) {
            result = std::string(key, len);
        } else {
            result = std::unexpected(ShakeError{.message = "the server showed no host key"});
        }
        libssh2_session_disconnect(session, "grab has read the host key");
        libssh2_session_free(session);
    }
    close_socket(s);
    return result;
}

} // namespace

KnownHost check_known_host(std::string_view known_hosts_text, std::string_view host, int port, std::string_view blob) {
    init_once();
    LIBSSH2_SESSION* session = libssh2_session_init_ex(nullptr, nullptr, nullptr, nullptr);
    if (!session) throw Error(budget::Failure::other, "can't check the host key");
    try {
        const KnownHost known = check_known(session, known_hosts_text, host, port, blob);
        libssh2_session_free(session);
        return known;
    } catch (...) {
        libssh2_session_free(session);
        throw;
    }
}

std::expected<std::vector<HostKey>, std::string> scan_host_keys(const Shake& shake, std::string_view where,
                                                               const std::stop_token& stop) {
    std::vector<std::string_view> left(scan_algorithms.begin(), scan_algorithms.end());
    std::vector<HostKey> keys;
    while (!left.empty()) {
        if (stop.stop_requested()) return util::fail("cancelled");
        std::string offer;
        for (const auto alg : left) offer += (offer.empty() ? "" : ",") + std::string(alg);
        auto blob = shake(offer);
        if (!blob) {
            // The types left aren't the server's: every key it has of the others was read.
            if (blob.error().no_match && !keys.empty()) break;
            if (blob.error().no_match) {
                return util::failf("{} has no host key grab can check (ssh-ed25519, ECDSA, or RSA with SHA-2 "
                                   "signatures), or only key exchanges too old for it: {}",
                                   where, blob.error().message);
            }
            return util::failf("can't read the host keys of {}: {}", where, blob.error().message);
        }
        auto key = host_key(*blob);
        if (!key) return util::failf("{} showed a host key grab can't read", where);
        // An RSA key answers both rsa-sha2 algorithms; a type that answers none of those asked
        // (or was found already) would never end the scan.
        auto covered = [&](std::string_view alg) {
            return key->type == "ssh-rsa" ? alg.starts_with("rsa-sha2-") : alg == key->type;
        };
        if (std::ranges::none_of(left, covered)) {
            return util::failf("{} showed a {} key when grab asked for {}", where, key->type, offer);
        }
        std::erase_if(left, covered);
        keys.push_back(std::move(*key));
    }
    return keys;
}

std::expected<std::vector<HostKey>, std::string> scan_host_keys(const std::string& host, int port,
                                                               std::chrono::milliseconds timeout, std::stop_token stop) {
    init_once();
    Target t;
    t.host = host;
    t.port = port;
    t.connect_timeout = timeout;
    t.timeout = timeout;
    const Shake shake = [&t](std::string_view offer) { return read_host_key(t, offer); };
    return scan_host_keys(shake, std::format("{}:{}", host, port), stop);
}

// libssh2 callbacks reach the Connection through the session's abstract pointer.
struct SessionAccess {
    static LIBSSH2_RECV_FUNC(recv) {
        auto* self = static_cast<Connection*>(*abstract);
        auto n = ::recv(static_cast<socket_t>(socket), static_cast<char*>(buffer), static_cast<int>(length), flags);
        if (n < 0) {
            int e = last_socket_error();
#ifdef _WIN32
            return (e == WSAEWOULDBLOCK || e == WSAEINTR) ? -EAGAIN : -EIO;
#else
            return (e == EAGAIN || e == EWOULDBLOCK || e == EINTR) ? -EAGAIN : -e;
#endif
        }
        self->received_.fetch_add(static_cast<std::uint64_t>(n), std::memory_order_relaxed);
        return n;
    }
    static LIBSSH2_USERAUTH_KBDINT_RESPONSE_FUNC(answer) {
        (void)name, (void)name_len, (void)instruction, (void)instruction_len, (void)prompts;
        const std::string& password = static_cast<Connection*>(*abstract)->password_;
        for (int i = 0; i < num_prompts; ++i) {
            // libssh2 frees each answer with its allocator (malloc by default).
            auto* text = static_cast<char*>(std::malloc(password.size() + 1));
            if (!text) continue;
            std::memcpy(text, password.c_str(), password.size() + 1);
            responses[i].text = text;
            responses[i].length = static_cast<unsigned int>(password.size());
        }
    }
};

Connection::Connection(const Target& target) {
    init_once();
    try {
        open(target);
    } catch (...) {
        close(); // a constructor that throws gets no destructor
        throw;
    }
}

Connection::~Connection() { close(); }

void Connection::close() {
    if (session_) {
        libssh2_session_disconnect(session_, "grab is done");
        libssh2_session_free(session_);
        session_ = nullptr;
    }
    if (socket_ != ~std::uintptr_t{0}) {
        close_socket(static_cast<socket_t>(socket_));
        socket_ = ~std::uintptr_t{0};
    }
    std::fill(password_.begin(), password_.end(), '\0');
}

void Connection::open(const Target& target) {
    auto started = std::chrono::steady_clock::now();
    socket_ = static_cast<std::uintptr_t>(connect_tcp(target));
    password_ = target.password;
    session_ = libssh2_session_init_ex(nullptr, nullptr, nullptr, this);
    if (!session_) fail(budget::Failure::other, "can't start an SSH session");
    libssh2_session_callback_set2(session_, LIBSSH2_CALLBACK_RECV,
                                  reinterpret_cast<libssh2_cb_generic*>(&SessionAccess::recv));
    libssh2_session_set_blocking(session_, 1);
    libssh2_session_set_timeout(session_, ms_or_none(target.timeout));

    std::string text;
    if (std::ifstream in{target.known_hosts, std::ios::binary}) text.assign(std::istreambuf_iterator<char>(in), {});
    std::string hostkeys = pinned_algorithms(text, target.host, target.port);
    if (hostkeys.empty())
        throw Error(budget::Failure::fatal, std::format("{}'s host key isn't trusted yet", target.host));
    libssh2_session_method_pref(session_, LIBSSH2_METHOD_KEX, std::string(kex_algorithms).c_str());
    libssh2_session_method_pref(session_, LIBSSH2_METHOD_CRYPT_CS, std::string(cipher_algorithms).c_str());
    libssh2_session_method_pref(session_, LIBSSH2_METHOD_CRYPT_SC, std::string(cipher_algorithms).c_str());
    libssh2_session_method_pref(session_, LIBSSH2_METHOD_MAC_CS, std::string(mac_algorithms).c_str());
    libssh2_session_method_pref(session_, LIBSSH2_METHOD_MAC_SC, std::string(mac_algorithms).c_str());
    if (libssh2_session_method_pref(session_, LIBSSH2_METHOD_HOSTKEY, hostkeys.c_str()) != 0)
        fail(budget::Failure::fatal, "grab can't check this kind of host key (" + hostkeys + ")");

    if (int rc = libssh2_session_handshake(session_, static_cast<socket_t>(socket_)); rc != 0) {
        // No common algorithm can't get better by trying again; a dropped or reset connection
        // before the login is the server turning grab away (MaxStartups, a full Storage Box).
        bool hopeless = rc == LIBSSH2_ERROR_KEX_FAILURE || rc == LIBSSH2_ERROR_METHOD_NOT_SUPPORTED;
        bool slow = rc == LIBSSH2_ERROR_TIMEOUT;
        fail(hopeless ? budget::Failure::fatal : slow ? budget::Failure::other : budget::Failure::refused,
             "SSH handshake with " + target.host + " failed");
    }
    check_host_key(target, text);
    log_in(target);
    login_time_ =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
}

void Connection::fail(budget::Failure kind, std::string_view what) const {
    throw Error(kind, session_ ? std::format("{}: {}", what, last_error(session_)) : std::string(what));
}

void Connection::check_host_key(const Target& target, std::string_view known_hosts_text) {
    std::size_t len = 0;
    int type = 0;
    const char* key = libssh2_session_hostkey(session_, &len, &type);
    switch (type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
    case LIBSSH2_HOSTKEY_TYPE_ED25519: break;
    default: throw Error(budget::Failure::fatal, std::format("{} offered a host key grab can't check", target.host));
    }
    // known_hosts was read by open() (libssh2's own reader takes a narrow path).
    switch (check_known(session_, known_hosts_text, target.host, target.port, std::string_view(key, len))) {
    case KnownHost::match: return;
    case KnownHost::mismatch:
        throw Error(budget::Failure::fatal,
                    std::format("{}'s host key has changed: grab won't log in until you trust it again", target.host));
    case KnownHost::unknown: break;
    }
    throw Error(budget::Failure::fatal, std::format("{}'s host key isn't trusted yet", target.host));
}

// Tries what the target has, in rclone's order: ssh-agent's keys, the key file, the password.
void Connection::log_in(const Target& target) {
    const char* user = target.user.c_str();
    auto ulen = static_cast<unsigned>(target.user.size());
    std::vector<std::string> tried; // why each way failed, for the message
    if (target.agent) {
        LIBSSH2_AGENT* agent = libssh2_agent_init(session_);
        if (agent && libssh2_agent_connect(agent) == 0) {
            bool ok = false;
            if (libssh2_agent_list_identities(agent) == 0) {
                libssh2_agent_publickey* identity = nullptr;
                libssh2_agent_publickey* previous = nullptr;
                while (!ok && libssh2_agent_get_identity(agent, &identity, previous) == 0) {
                    ok = libssh2_agent_userauth(agent, user, identity) == 0;
                    previous = identity;
                }
            }
            libssh2_agent_disconnect(agent);
            libssh2_agent_free(agent);
            if (ok) return;
            tried.emplace_back("it accepted none of ssh-agent's keys");
        } else {
            if (agent) libssh2_agent_free(agent);
            tried.emplace_back("ssh-agent can't be reached (is the OpenSSH Authentication Agent running?)");
        }
    }
    if (!target.key_file.empty()) {
        // Read here (libssh2's own reader takes a narrow path).
        std::ifstream in{target.key_file, std::ios::binary};
        if (in) {
            std::string key{std::istreambuf_iterator<char>(in), {}};
            const char* passphrase = target.key_passphrase.empty() ? nullptr : target.key_passphrase.c_str();
            int rc = libssh2_userauth_publickey_frommemory(session_, user, target.user.size(), nullptr, 0,
                                                           key.data(), key.size(), passphrase);
            std::fill(key.begin(), key.end(), '\0');
            if (rc == 0) return;
            tried.push_back("it refused the key file (" + last_error(session_) + ")");
        } else {
            tried.push_back("the key file can't be read: " + util::path_to_utf8(target.key_file));
        }
    }
    if (!password_.empty()) {
        const char* listed = libssh2_userauth_list(session_, user, ulen);
        std::string methods = listed ? listed : "";
        int rc = -1;
        if (methods.find("password") != std::string::npos)
            rc = libssh2_userauth_password_ex(session_, user, ulen, password_.c_str(),
                                              static_cast<unsigned>(password_.size()), nullptr);
        if (rc != 0 && methods.find("keyboard-interactive") != std::string::npos)
            rc = libssh2_userauth_keyboard_interactive_ex(session_, user, ulen, &SessionAccess::answer);
        if (rc == 0) return;
        tried.emplace_back("it refused the password");
    }
    if (tried.empty()) tried.emplace_back("there is no agent, key or password to log in with");
    std::string why;
    for (const auto& t : tried) why += (why.empty() ? "" : "; ") + t;
    throw Error(budget::Failure::fatal, std::format("{} refused the login: {}", target.host, why));
}

std::unique_ptr<sftp::Channel> Connection::open_sftp() {
    LIBSSH2_CHANNEL* ch =
        libssh2_channel_open_ex(session_, "session", 7, window_size, LIBSSH2_CHANNEL_PACKET_DEFAULT, nullptr, 0);
    if (!ch) fail(budget::Failure::other, "can't open a channel");
    if (libssh2_channel_process_startup(ch, "subsystem", 9, "sftp", 4) != 0) {
        libssh2_channel_free(ch);
        fail(budget::Failure::fatal, "the server doesn't offer SFTP");
    }
    return std::make_unique<ChannelStream>(session_, ch);
}

std::expected<ExecResult, ExecError> Connection::exec(std::string_view command, const ExecOptions& options) {
    SshCommand channel(session_, static_cast<socket_t>(socket_), received_, dropped_);
    return run_command(channel, command, options);
}

Output Connection::run(std::string_view command, std::size_t limit) {
    Output result;
    ExecOptions options;
    options.out = [&](std::string_view piece) {
        std::size_t room = limit - std::min(limit, result.out.size());
        result.out.append(piece.substr(0, room));
        result.cut = result.cut || piece.size() > room;
        return true; // read on to the end, for the exit status
    };
    options.err_limit = limit;
    auto done = exec(command, options);
    if (!done) throw Error(budget::Failure::other, done.error().message);
    result.err = std::move(done->err);
    result.cut = result.cut || done->err_cut;
    result.status = done->status;
    return result;
}

std::expected<ExecResult, ExecError> run_command(CommandChannel& channel, std::string_view command,
                                                 const ExecOptions& options) {
    using Clock = std::chrono::steady_clock;
    using std::chrono::milliseconds;
    auto seconds = [](milliseconds d) { return std::chrono::duration<double>(d).count(); };
    const auto started = Clock::now();
    const bool has_deadline = options.deadline > milliseconds::zero();
    ExecResult result;
    enum class Step { start, eof, running, closing } step = Step::start;
    bool refused = false;
    std::uint64_t seen = channel.received();
    auto heard = started;  // when the server last sent anything
    auto pinged = started; // when the last keepalive went out
    std::vector<char> buffer(std::size_t{64} * 1024);
    // Cut short: the command is told to end and its channel dropped.
    auto cut_short = [&](ExecFailure kind, std::string message) {
        channel.terminate();
        channel.drop(false);
        return std::unexpected(ExecError{.kind = kind, .message = std::move(message), .err = std::move(result.err)});
    };
    for (;;) {
        if (options.stop.stop_requested()) return cut_short(ExecFailure::stopped, "stopped");
        auto now = Clock::now();
        if (has_deadline && now - started >= options.deadline)
            return cut_short(ExecFailure::deadline,
                             std::format("the command took longer than {:g} s", seconds(options.deadline)));
        bool busy = false; // something moved: round again before waiting
        if (step == Step::start) {
            if (auto ran = channel.start(command)) {
                refused = !*ran;
                step = refused ? Step::closing : Step::eof;
                busy = true;
            }
        } else if (step == Step::eof) {
            // Right after the exec request: nothing comes on stdin.
            if (channel.send_eof()) {
                step = Step::running;
                busy = true;
            }
        } else if (step == Step::running) {
            // Both streams in turn, so a command filling one can't stall on the other.
            if (std::size_t n = channel.read(false, buffer)) {
                busy = true;
                std::string_view piece(buffer.data(), n);
                bool over = options.out_limit > 0 && result.out_bytes + n > options.out_limit;
                if (over) piece = piece.substr(0, static_cast<std::size_t>(options.out_limit - result.out_bytes));
                result.out_bytes += piece.size();
                if (!piece.empty() && options.out && !options.out(piece)) over = true;
                if (over) return cut_short(ExecFailure::too_much, "the command's output is too large");
            }
            if (std::size_t n = channel.read(true, buffer)) {
                busy = true;
                std::size_t room = options.err_limit - std::min(options.err_limit, result.err.size());
                result.err.append(buffer.data(), std::min(room, n));
                result.err_cut = result.err_cut || n > room;
            }
            if (!busy && channel.ended()) {
                step = Step::closing;
                busy = true;
            }
        } else if (channel.close()) {
            break;
        }
        if (auto now_seen = channel.received(); now_seen != seen) {
            seen = now_seen;
            heard = now;
        }
        if (busy) continue;

        // Nothing to do until the server sends something. While it's silent keepalives go out,
        // and each gets an answer: a link silent through them is dead.
        if (now - heard >= options.dead_after) {
            channel.drop(true);
            throw Error(budget::Failure::other,
                        std::format("the server stopped answering (nothing for {:g} s)", seconds(options.dead_after)));
        }
        if (now - std::max(heard, pinged) >= options.keepalive) {
            channel.keepalive();
            pinged = now;
        }
        auto due = std::min(std::max(heard, pinged) + options.keepalive, heard + options.dead_after);
        if (has_deadline) due = std::min(due, started + options.deadline);
        auto most = std::chrono::duration_cast<milliseconds>(due - now);
        channel.wait(std::clamp(most, milliseconds(1), milliseconds(1000)));
    }
    if (refused) {
        std::string why = channel.refusal();
        return std::unexpected(ExecError{.kind = ExecFailure::refused,
                                         .message = why.empty() ? "the server refused to run the command"
                                                                : "the server refused to run the command: " + why,
                                         .err = {}});
    }
    result.status = channel.exit_status();
    result.signal = channel.exit_signal();
    return result;
}

std::string Connection::algorithms() const {
    auto method = [this](int which) {
        const char* m = libssh2_session_methods(session_, which);
        return std::string(m ? m : "?");
    };
    return std::format("{} {} {}", method(LIBSSH2_METHOD_KEX), method(LIBSSH2_METHOD_HOSTKEY),
                       method(LIBSSH2_METHOD_CRYPT_SC));
}

} // namespace grab::ssh
