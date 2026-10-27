#pragma once

// A server's SSH connections, kept open between uses: a download's thread borrows one (a Lease),
// reads through it and gives it back for the next. A login costs about 1 s on Main and 1-3.5 s
// on a Storage Box, which rclone paid for every byte range; here it is paid once per connection.
//
// At most `limit` connections are open (idle ones count: Storage Boxes count them against the
// account), at most `logins_at_once` log in at the same time (OpenSSH's MaxStartups; Main drops
// connections above 5 and runs fail2ban), a login the server turned away is tried again after a
// pause, and a connection idle for a minute is closed.
//
// libssh2 sessions aren't thread-safe; a Lease gives one thread a session until it's returned.

#include "sftp.hpp"
#include "ssh.hpp"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace grab::ssh {

// One pooled login and what borrowers use of it.
class Session {
public:
    virtual ~Session() = default;
    [[nodiscard]] virtual sftp::Client& sftp() = 0;
    // Runs a command (find, md5sum) on this login: Connection::exec, or canned output in tests.
    // Where there is none, the server refuses it.
    [[nodiscard]] virtual std::expected<ExecResult, ExecError> exec(std::string_view command,
                                                                    const ExecOptions& options);
    // The login itself; nullptr where there is none (tests).
    [[nodiscard]] virtual Connection* connection() { return nullptr; }
    // Whether the next borrower may have it: no SFTP reply still on its way, which would reach
    // a request the next one never made, and no command cut short on it (Connection::dropped).
    [[nodiscard]] virtual bool reusable() { return true; }
};

// Logs in to `target` and starts SFTP. Throws Error.
[[nodiscard]] std::unique_ptr<Session> open_session(const Target& target);

// What a server answered grab's find check (ssh_find.hpp): whether its GNU find works as grab
// needs it, and what it said, in a few words for -v and errors.
struct FindCheck {
    bool works = false;
    std::string answer;
    bool lasting = true; // false: no answer in time, which may pass (Pool::find_check asks again)
    bool operator==(const FindCheck&) const = default;
};

class Pool {
public:
    using Clock = std::chrono::steady_clock;
    using Connect = std::function<std::unique_ptr<Session>()>; // throws Error

    struct Options {
        int limit = 4;
        int logins_at_once = 4;
        std::chrono::milliseconds idle_close{60'000};
        // Pauses before each new try of a login the server turned away (budget::Failure::refused).
        std::vector<std::chrono::milliseconds> retry_refused{std::chrono::milliseconds{500}, std::chrono::seconds{1},
                                                             std::chrono::seconds{2}, std::chrono::seconds{4},
                                                             std::chrono::seconds{8}};
        bool idle_timer = true; // the pool's own thread closes idle connections (tests call close_idle)
        // How long find_check keeps an answer that isn't lasting.
        std::chrono::milliseconds find_check_expiry{std::chrono::minutes(5)};
    };

    Pool(Connect connect, Options options);
    // Every Lease must be gone first.
    ~Pool();
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    class Lease {
    public:
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        ~Lease();
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Session& operator*() const { return *session_; }
        Session* operator->() const { return session_.get(); }
        // The session broke (sftp::Broken, a lost connection): it's closed instead of reused.
        void broken() { broken_ = true; }
        // Gives it back now.
        void release();

    private:
        friend class Pool;
        Lease(Pool* pool, std::unique_ptr<Session> session) : pool_(pool), session_(std::move(session)) {}
        Pool* pool_ = nullptr;
        std::unique_ptr<Session> session_;
        bool broken_ = false;
    };

    // A session: an idle one, else a new login once there is room under the limit and among the
    // logins at once, else waiting for one to come back. nullopt once `stop` is requested. Throws
    // Error: a fatal one at once, a refusal after the retries.
    [[nodiscard]] std::optional<Lease> acquire(std::stop_token stop = {});

    // Lowering it closes idle connections over it; borrowed ones close when they come back.
    void set_limit(int limit);
    [[nodiscard]] int limit() const;
    // Connections open: idle, borrowed and logging in.
    [[nodiscard]] int open() const;
    [[nodiscard]] int idle() const;
    // Closes connections idle longer than idle_close at `now`.
    void close_idle(Clock::time_point now);

    // The find check of this pool's server, asked once: `check` runs on the first call (one at a
    // time: a second caller waits for the first one's answer, or until its `stop`, then nullopt)
    // and its answer is kept for the pool's life, so a program asks each server once; one that
    // isn't lasting only for find_check_expiry. nullopt (stopped) isn't kept; an Error `check`
    // throws goes to the caller and isn't kept either, and a caller waiting runs its own check.
    [[nodiscard]] std::optional<FindCheck> find_check(std::stop_token stop,
                                                      const std::function<std::optional<FindCheck>()>& check);

private:
    struct Idle {
        std::unique_ptr<Session> session;
        Clock::time_point since;
    };
    void give_back(std::unique_ptr<Session> session, bool broken);
    [[nodiscard]] int open_locked() const { return leased_ + logging_in_ + static_cast<int>(idle_.size()); }

    Connect connect_;
    Options options_;
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::deque<Idle> idle_; // most recently used at the back
    int leased_ = 0;
    int logging_in_ = 0;
    bool checking_ = false; // a find check is running
    std::optional<FindCheck> find_check_;
    Clock::time_point find_checked_;
    std::jthread timer_;
};

// The pools of the servers on grab's engine, one per server, made on first use, for a program
// (the window, a command) to share among its downloads. A server whose login changed (a password
// edited in Settings) gets a new pool; downloads on the old one keep it until they end.
// Thread-safe.
class Pools {
public:
    // How downloads use their pools: refusals are left to fetch, which backs off across all its
    // streams and learns the server's limit (conn_budget); idle connections close after 15 s, as
    // rclone's own folder copies and listings still open theirs beside them (phase 2).
    [[nodiscard]] static Pool::Options download_options(int limit);

    // `limit`: the server's connection budget now.
    [[nodiscard]] std::shared_ptr<Pool> get(const std::string& server, const Target& target, int limit);

private:
    struct Entry {
        Target target;
        std::shared_ptr<Pool> pool;
    };
    std::mutex mutex_;
    std::map<std::string, Entry> pools_;
};

} // namespace grab::ssh
