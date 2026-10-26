#pragma once

// grab's SSH connections (libssh2 on OpenSSL), replacing rclone's and ssh.exe's for the rclone
// replacement (RCLONE-REPLACEMENT.md). A Connection logs in once and then carries SFTP channels
// (sftp.hpp) and commands (find, md5sum) for as long as it stays open.
//
// Strict host keys: the server must prove a key grab's known_hosts holds for it; an unknown key or
// a changed one ends the connection before any secret is sent. Only modern algorithms are offered
// (OpenSSH's defaults: no SHA-1 MACs, no CBC or RC4 ciphers, no ssh-rsa or DSA host keys).
//
// Blocking: one thread per Connection, which no other thread touches.

#include "conn_budget.hpp"
#include "sftp.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

struct _LIBSSH2_SESSION;
struct _LIBSSH2_CHANNEL;

namespace grab::ssh {

struct Target {
    std::string host;
    int port = 22;
    std::string user;
    std::filesystem::path known_hosts; // grab's: the server's key must be in it
    // How to log in: the Windows OpenSSH agent, a key file (OpenSSH or PEM format), or a
    // password (also answers keyboard-interactive prompts). Secrets stay in memory.
    bool agent = false;
    std::filesystem::path key_file;
    std::string key_passphrase;
    std::string password;
    std::chrono::milliseconds timeout{30'000}; // connecting, and any single wait after that
};

// Why a connection failed, in the terms conn_budget uses: refused (the server turned it away
// before the login: MaxStartups, a full Storage Box; try again later), fatal (trying again can't
// help: unknown or changed host key, login refused), other (network, timeouts).
struct Error : std::runtime_error {
    Error(budget::Failure kind, const std::string& what) : std::runtime_error(what), kind(kind) {}
    budget::Failure kind;
};

// A command's result: what it wrote to stdout and stderr (each cut at the limit given, `cut`
// then set) and its exit status (-1 when the server didn't say).
struct Output {
    std::string out, err;
    int status = -1;
    bool cut = false;
};

class Connection {
public:
    // Connects, checks the host key, logs in. Throws Error.
    explicit Connection(const Target& target);
    ~Connection();
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    // A channel running the server's SFTP subsystem, for an sftp::Client. It must be destroyed
    // before its Connection. Throws Error.
    [[nodiscard]] std::unique_ptr<sftp::Channel> open_sftp();
    // Runs `command` through the server's shell and waits for it. Throws Error.
    [[nodiscard]] Output run(std::string_view command, std::size_t limit = 64 * 1024 * 1024);

    // What was agreed, for -v: "curve25519-sha256 ssh-ed25519 chacha20-poly1305@openssh.com".
    [[nodiscard]] std::string algorithms() const;
    // Bytes received on the socket so far (SSH framing included).
    [[nodiscard]] std::uint64_t received() const { return received_.load(std::memory_order_relaxed); }
    // How long the connect, key exchange and login took.
    [[nodiscard]] std::chrono::milliseconds login_time() const { return login_time_; }

private:
    void open(const Target& target);
    void close();
    [[noreturn]] void fail(budget::Failure kind, std::string_view what) const;
    void check_host_key(const Target& target);
    void log_in(const Target& target);

    std::uintptr_t socket_ = ~std::uintptr_t{0};
    _LIBSSH2_SESSION* session_ = nullptr;
    std::string password_; // the session's abstract points here (keyboard-interactive)
    std::atomic<std::uint64_t> received_{0};
    std::chrono::milliseconds login_time_{0};
    friend struct SessionAccess;
};

// Algorithms offered, best first (exposed for tests and -v).
inline constexpr std::string_view kex_algorithms =
    "mlkem768x25519-sha256,curve25519-sha256,curve25519-sha256@libssh.org,ecdh-sha2-nistp256,ecdh-sha2-nistp384,"
    "ecdh-sha2-nistp521,diffie-hellman-group-exchange-sha256,diffie-hellman-group16-sha512,"
    "diffie-hellman-group18-sha512,diffie-hellman-group14-sha256";
inline constexpr std::string_view cipher_algorithms =
    "chacha20-poly1305@openssh.com,aes128-gcm@openssh.com,aes256-gcm@openssh.com,aes128-ctr,aes192-ctr,aes256-ctr";
inline constexpr std::string_view mac_algorithms =
    "hmac-sha2-256-etm@openssh.com,hmac-sha2-512-etm@openssh.com,hmac-sha2-256,hmac-sha2-512";

// The host key algorithms to ask for, from the key types known_hosts holds for host:port
// ("[host]:port" lines, or "host" for port 22): the server then proves a key grab has.
// Empty when none is pinned.
[[nodiscard]] std::string pinned_algorithms(std::string_view known_hosts_text, std::string_view host, int port);

} // namespace grab::ssh
