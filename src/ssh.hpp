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

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

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
    // rclone's connection flags, as engine::ssh_target takes them from common_flags (review G189):
    // the local address to connect from (--bind; empty: any), the time to reach the server
    // (--contimeout) and any single wait after that (--timeout). A time of 0 is no limit.
    std::string bind;
    std::chrono::milliseconds connect_timeout{30'000};
    std::chrono::milliseconds timeout{30'000};
    bool operator==(const Target&) const = default;
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

// How Connection::exec runs a command.
struct ExecOptions {
    // Each piece of stdout as it arrives (valid during the call); false stops the command
    // (ExecFailure::too_much). Without one, stdout is read and thrown away.
    std::function<bool(std::string_view)> out;
    std::uint64_t out_limit = 0;                    // stdout bytes it may send (0: no cap), then it's stopped
    std::size_t err_limit = std::size_t{64} * 1024; // stderr kept; the rest is read and thrown away
    std::stop_token stop;                           // looked at least once a second
    std::chrono::milliseconds deadline{0};          // for the whole command (0: none)
    // While the server sends nothing, a keepalive goes out this often (libssh2 sends at most one
    // every 2 s). Each gets an answer, so a link that has sent nothing for `dead_after` is dead:
    // a silent but live command is never taken for one.
    std::chrono::milliseconds keepalive{15'000};
    std::chrono::milliseconds dead_after{45'000};
};

// A command that ran to its end.
struct ExecResult {
    std::string err;             // the first err_limit bytes of stderr
    bool err_cut = false;        // there was more
    int status = -1;             // exit status, or -1 (a signal ended it, or the server sent none)
    std::string signal;          // the signal that ended it ("TERM"), if one did
    std::uint64_t out_bytes = 0; // stdout bytes, every one given to `out`
};

// Why a command didn't run to its end. After `refused` the connection is as it was. After the
// others the command was told to end (SIGTERM, which OpenSSH delivers since 7.9) and its channel
// dropped without waiting for it, so the connection isn't used again (Connection::dropped).
enum class ExecFailure {
    refused,  // the server refused the exec request (an SFTP-only account) or a channel for it
              // (MaxSessions 1): SFTP still works
    stopped,  // the stop token
    deadline, // ExecOptions::deadline passed
    too_much, // stdout passed out_limit, or `out` said stop
};
struct ExecError {
    ExecFailure kind = ExecFailure::refused;
    std::string message;
    std::string err; // stderr until then
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
    // Runs `command` through the server's shell. Its stdin ends at once (EOF right after the exec
    // request), so a command reading it ends instead of waiting forever: cat, or the sftp-server
    // an internal-sftp account runs for any command. Throws Error when the link fails or goes
    // dead; the connection is dropped then too.
    [[nodiscard]] std::expected<ExecResult, ExecError> exec(std::string_view command,
                                                            const ExecOptions& options = {});
    // exec, collected and read to the end. Throws Error, also when the server refuses it.
    [[nodiscard]] Output run(std::string_view command, std::size_t limit = 64 * 1024 * 1024);
    // A command was cut short on it or its link failed: it isn't used any more (a pooled
    // Session isn't reused).
    [[nodiscard]] bool dropped() const { return dropped_; }

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
    void check_host_key(const Target& target, std::string_view known_hosts_text);
    void log_in(const Target& target);

    std::uintptr_t socket_ = ~std::uintptr_t{0};
    _LIBSSH2_SESSION* session_ = nullptr;
    std::string password_; // the session's abstract points here (keyboard-interactive)
    std::atomic<std::uint64_t> received_{0};
    std::chrono::milliseconds login_time_{0};
    bool dropped_ = false;
    friend struct SessionAccess;
};

// The channel a command runs on, as exec drives it: an SSH channel, or a fake in tests. No call
// waits except wait(); those that fail throw Error.
class CommandChannel {
public:
    virtual ~CommandChannel() = default;
    // Opens the channel and sends the exec request: true once the command runs, false when the
    // server refused it or the channel, nullopt while that isn't known yet.
    [[nodiscard]] virtual std::optional<bool> start(std::string_view command) = 0;
    // What the server said when it refused: "Channel open failure (connect failed)" for the
    // channel; empty for the exec request, whose refusal carries no reason.
    [[nodiscard]] virtual std::string refusal() = 0;
    // Ends the command's stdin: false when it couldn't go out yet (call again).
    [[nodiscard]] virtual bool send_eof() = 0;
    // Bytes of stdout (stderr if `err`) that are there now; 0 when none are.
    [[nodiscard]] virtual std::size_t read(bool err, std::span<char> buffer) = 0;
    // The output ended (EOF) and every byte of it was read.
    [[nodiscard]] virtual bool ended() = 0;
    // Closes the channel after ended() or a refused start: true once the server closed its side
    // too. Then the exit status (-1 when there is none) and the signal that ended the command.
    [[nodiscard]] virtual bool close() = 0;
    [[nodiscard]] virtual int exit_status() = 0;
    [[nodiscard]] virtual std::string exit_signal() = 0;
    // Asks the server to end the command with SIGTERM.
    virtual void terminate() = 0;
    // Lets go of the channel without waiting for the command; the connection isn't used again.
    // `dead`: the link is gone, so nothing more is sent or awaited on it.
    virtual void drop(bool dead) = 0;
    // A keepalive, which the server answers (OpenSSH: REQUEST_FAILURE, traffic all the same).
    virtual void keepalive() = 0;
    // Bytes received on the connection so far.
    [[nodiscard]] virtual std::uint64_t received() = 0;
    // Waits at most `most` for the server to send something.
    virtual void wait(std::chrono::milliseconds most) = 0;
};

// Connection::exec's loop, over any channel (exposed for tests).
[[nodiscard]] std::expected<ExecResult, ExecError> run_command(CommandChannel& channel, std::string_view command,
                                                               const ExecOptions& options);

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

// ---- host keys -------------------------------------------------------------------------------

// A server's public host key, as a scan read it.
struct HostKey {
    std::string type;        // the type the key names itself: "ssh-ed25519", "ecdsa-sha2-nistp521", "ssh-rsa"
    std::string key;         // its blob in base64, as known_hosts holds it
    std::string fingerprint; // "SHA256:...", as ssh-keygen -l shows it
    bool operator==(const HostKey&) const = default;
};

// The type a public key blob names (its first field: "ssh-rsa" for an RSA key, whichever
// signature was agreed); empty when it names none.
[[nodiscard]] std::string key_type(std::string_view blob);
// "SHA256:" and the blob's SHA-256 in base64 without padding, as ssh-keygen -l shows it.
[[nodiscard]] std::string fingerprint(std::string_view blob);
// The HostKey of a blob; nullopt when it names no type.
[[nodiscard]] std::optional<HostKey> host_key(std::string_view blob);
// The known_hosts line that pins `key` for host:port: "[host]:port type key", the plain host for
// port 22, never hashed, so rclone's Go knownhosts and OpenSSH read it as well as grab.
[[nodiscard]] std::string known_host_line(std::string_view host, int port, const HostKey& key);

// What known_hosts says of the key a server at host:port presented, as a login checks it
// (libssh2's matching: "[host]:port", then the plain host; hashed names too).
enum class KnownHost {
    match,    // a pin of this type holds this key
    mismatch, // a pin of this type holds another key: the key changed
    unknown,  // nothing of this type is pinned (or the type isn't one grab checks)
};
[[nodiscard]] KnownHost check_known_host(std::string_view known_hosts_text, std::string_view host, int port,
                                         std::string_view blob);

// The host key algorithms a scan asks for, best first: one handshake gets the first of them the
// server has, the next one the first of those left, and so on.
inline constexpr std::array<std::string_view, 6> scan_algorithms{
    "ssh-ed25519",         "ecdsa-sha2-nistp256", "ecdsa-sha2-nistp384",
    "ecdsa-sha2-nistp521", "rsa-sha2-512",        "rsa-sha2-256"};

// One scanning handshake's outcome: the blob of the host key the server showed for `offer`, else
// why not; `no_match` when it ended only for want of an algorithm both sides know.
struct ShakeError {
    bool no_match = false;
    std::string message;
};
using Shake = std::function<std::expected<std::string, ShakeError>(std::string_view offer)>;

// Every host key `host`:`port` has of the types grab checks (scan_algorithms), read without a
// login and without looking at any known_hosts: handshakes one after another, each offering the
// algorithms whose key wasn't found yet (an RSA key covers both rsa-sha2 ones); the first one the
// server can't agree on ends the scan (Main: 2 handshakes, a Storage Box 4, at most 6). Each
// handshake has `timeout`; `stop` is looked at between them ("cancelled"). Nothing is written. An
// error when no key could be read (the server has only key types or key exchanges grab doesn't
// support: SHA-1 ssh-rsa, DSA, old Diffie-Hellman) or a handshake fails for another reason.
[[nodiscard]] std::expected<std::vector<HostKey>, std::string> scan_host_keys(const std::string& host, int port,
                                                                             std::chrono::milliseconds timeout,
                                                                             std::stop_token stop = {});
// The same over any handshake (exposed for tests); `where` ("host:port") is for messages.
[[nodiscard]] std::expected<std::vector<HostKey>, std::string> scan_host_keys(const Shake& shake, std::string_view where,
                                                                             const std::stop_token& stop = {});

} // namespace grab::ssh
