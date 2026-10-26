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

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
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

// A TCP connection to host:port within `timeout`.
socket_t connect_tcp(const Target& t) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* found = nullptr;
    if (getaddrinfo(t.host.c_str(), std::to_string(t.port).c_str(), &hints, &found) != 0 || !found)
        throw Error(budget::Failure::other, std::format("can't find the server {}", t.host));
    std::string why = "no address";
    socket_t s = no_socket;
    for (addrinfo* a = found; a && s == no_socket; a = a->ai_next) {
        socket_t c = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (c == no_socket) continue;
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
                auto ms = t.timeout.count();
                timeval tv{static_cast<long>(ms / 1000), static_cast<long>((ms % 1000) * 1000)};
                if (select(static_cast<int>(c + 1), nullptr, &w, &x, &tv) > 0 && FD_ISSET(c, &w)) {
                    int err = 0;
                    socklen_t len = sizeof err;
                    getsockopt(c, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
                    ok = err == 0;
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
    freeaddrinfo(found);
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

// Waits until the socket can move what libssh2 is blocked on (non-blocking mode), at most 1 s.
void wait_socket(socket_t s, LIBSSH2_SESSION* session) {
    int dir = libssh2_session_block_directions(session);
    fd_set r;
    fd_set w;
    FD_ZERO(&r);
    FD_ZERO(&w);
    if (dir & LIBSSH2_SESSION_BLOCK_INBOUND) FD_SET(s, &r);
    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) FD_SET(s, &w);
    timeval tv{1, 0};
    select(static_cast<int>(s + 1), &r, &w, nullptr, &tv);
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
    libssh2_session_set_timeout(session_, static_cast<long>(target.timeout.count()));

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
    check_host_key(target);
    log_in(target);
    login_time_ =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
}

void Connection::fail(budget::Failure kind, std::string_view what) const {
    throw Error(kind, session_ ? std::format("{}: {}", what, last_error(session_)) : std::string(what));
}

void Connection::check_host_key(const Target& target) {
    std::size_t len = 0;
    int type = 0;
    const char* key = libssh2_session_hostkey(session_, &len, &type);
    int keybit = 0;
    switch (type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA: keybit = LIBSSH2_KNOWNHOST_KEY_SSHRSA; break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: keybit = LIBSSH2_KNOWNHOST_KEY_ECDSA_256; break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: keybit = LIBSSH2_KNOWNHOST_KEY_ECDSA_384; break;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: keybit = LIBSSH2_KNOWNHOST_KEY_ECDSA_521; break;
    case LIBSSH2_HOSTKEY_TYPE_ED25519: keybit = LIBSSH2_KNOWNHOST_KEY_ED25519; break;
    default: throw Error(budget::Failure::fatal, std::format("{} offered a host key grab can't check", target.host));
    }
    // known_hosts is read here line by line (libssh2's own reader takes a narrow path).
    LIBSSH2_KNOWNHOSTS* known = libssh2_knownhost_init(session_);
    if (!known) fail(budget::Failure::other, "can't check the host key");
    if (std::ifstream in{target.known_hosts, std::ios::binary}) {
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            // A line libssh2 can't read (a key type it doesn't know) holds nothing it could match.
            (void)libssh2_knownhost_readline(known, line.c_str(), line.size(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);
        }
    }
    libssh2_knownhost* match = nullptr;
    int check = libssh2_knownhost_checkp(known, target.host.c_str(), target.port, key, len,
                                         LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | keybit, &match);
    libssh2_knownhost_free(known);
    if (check == LIBSSH2_KNOWNHOST_CHECK_MATCH) return;
    if (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH)
        throw Error(budget::Failure::fatal,
                    std::format("{}'s host key has changed: grab won't log in until you trust it again", target.host));
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

Output Connection::run(std::string_view command, std::size_t limit) {
    LIBSSH2_CHANNEL* ch =
        libssh2_channel_open_ex(session_, "session", 7, window_size, LIBSSH2_CHANNEL_PACKET_DEFAULT, nullptr, 0);
    if (!ch) fail(budget::Failure::other, "can't open a channel");
    std::string cmd(command);
    if (libssh2_channel_process_startup(ch, "exec", 4, cmd.c_str(), static_cast<unsigned>(cmd.size())) != 0) {
        libssh2_channel_free(ch);
        fail(budget::Failure::other, "the server didn't run the command");
    }
    // Both streams at once, so a command filling one can't stall on the other.
    Output result;
    libssh2_session_set_blocking(session_, 0);
    auto restore = [&] { libssh2_session_set_blocking(session_, 1); };
    std::vector<char> buf(64 * 1024);
    auto take = [&](std::string& to, int stream) {
        auto n = libssh2_channel_read_ex(ch, stream, buf.data(), buf.size());
        if (n > 0) {
            std::size_t room = limit > to.size() ? limit - to.size() : 0;
            to.append(buf.data(), std::min<std::size_t>(room, static_cast<std::size_t>(n)));
            if (static_cast<std::size_t>(n) > room) result.cut = true;
        }
        return n;
    };
    for (;;) {
        auto o = take(result.out, 0);
        auto e = take(result.err, SSH_EXTENDED_DATA_STDERR);
        if ((o < 0 && o != LIBSSH2_ERROR_EAGAIN) || (e < 0 && e != LIBSSH2_ERROR_EAGAIN)) {
            restore();
            libssh2_channel_free(ch);
            fail(budget::Failure::other, "reading the command's output failed");
        }
        if (o > 0 || e > 0) continue;
        if (libssh2_channel_eof(ch)) break;
        wait_socket(static_cast<socket_t>(socket_), session_);
    }
    while (libssh2_channel_close(ch) == LIBSSH2_ERROR_EAGAIN) wait_socket(static_cast<socket_t>(socket_), session_);
    while (libssh2_channel_wait_closed(ch) == LIBSSH2_ERROR_EAGAIN)
        wait_socket(static_cast<socket_t>(socket_), session_);
    result.status = libssh2_channel_get_exit_status(ch);
    restore();
    libssh2_channel_free(ch);
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
