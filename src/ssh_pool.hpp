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

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace grab::ssh {

// Pauses before each new try of a login the server turned away (budget::Failure::refused), for
// one call of Pool::acquire; nullopt: the pool's own (Options::retry_refused).
using Retry = std::optional<std::span<const std::chrono::milliseconds>>;
// Those of a caller a person waits on (Browse, a search, a list): a busy server, such as a full
// Storage Box, doesn't fail it at once. Downloads take none: fetch backs off across all its
// streams and learns the server's limit (conn_budget).
inline constexpr std::array<std::chrono::milliseconds, 4> interactive_retry{
    std::chrono::milliseconds{500}, std::chrono::milliseconds{1000}, std::chrono::milliseconds{2000},
    std::chrono::milliseconds{4000}};

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
    bool lasting = true;      // false: no answer in time, which may pass (Pool::find_check asks again)
    std::int64_t checked = 0; // when the server gave it (Unix seconds); 0: not known
    bool saved = false;       // kept by an earlier run (FindMemory, ssh_find.hpp), not asked in this one
    bool operator==(const FindCheck&) const = default;
};

// How long an answer that is lasting holds, in a pool and saved across runs: a server's find
// rarely comes or goes, and a "find works" gone stale is caught when find fails
// (engine::grab_find).
inline constexpr std::chrono::hours find_check_trust{24 * 7};
// How long an answer that isn't lasting holds in a pool, and how long any answer counts as just
// asked: find failing on one this fresh isn't worth asking again (engine::grab_find).
inline constexpr std::chrono::minutes find_check_fresh{5};

// The check's answers kept across runs (limits.json, engine::find_memory), so a command doesn't
// ask a server again each run: `load` gives the one saved for the server while it holds
// (find_check_trust; FindCheck::saved and `checked` set), `save` keeps a new lasting one or,
// with nullopt, drops the saved one. Either may be empty.
struct FindMemory {
    std::function<std::optional<FindCheck>()> load;
    std::function<void(const std::optional<FindCheck>&)> save;
};

// How a search or list goes on engine = grab (choose_lookup, ssh_find.hpp).
struct Lookup {
    bool find = false;        // find; else the walk
    std::string answer;       // what the check found, for -v; empty when it wasn't asked
    std::int64_t checked = 0; // when the server gave the answer (Unix seconds); 0: not known
    bool saved = false;       // the answer an earlier run kept
    bool asked = false;       // the check ran for this lookup (else its answer was known before)
    std::string redone = {};  // why it was asked again (find failing on the answer known before)
};

class Pool {
public:
    using Clock = std::chrono::steady_clock;
    using Connect = std::function<std::unique_ptr<Session>()>; // throws Error

    struct Options {
        int limit = 4;
        int logins_at_once = 4;
        std::chrono::milliseconds idle_close{60'000};
        // The most recently used idle connection stays this long instead, when that is longer: a
        // Browse click a minute later needs no new login.
        std::chrono::milliseconds keep_newest{0};
        // Pauses before each new try of a login the server turned away (budget::Failure::refused).
        std::vector<std::chrono::milliseconds> retry_refused{std::chrono::milliseconds{500}, std::chrono::seconds{1},
                                                             std::chrono::seconds{2}, std::chrono::seconds{4},
                                                             std::chrono::seconds{8}};
        bool idle_timer = true; // the pool's own thread closes idle connections (tests call close_idle)
        // How long find_check keeps an answer that isn't lasting, and one that is.
        std::chrono::milliseconds find_check_expiry{ssh::find_check_fresh};
        std::chrono::milliseconds find_check_trust{ssh::find_check_trust};
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
        // It sat idle in the pool: the server may have closed it meanwhile (ClientAliveInterval, a
        // NAT timeout), so a read-only call that finds it broken is worth one more try.
        [[nodiscard]] bool reused() const { return reused_; }
        // Gives it back now.
        void release();

    private:
        friend class Pool;
        Lease(Pool* pool, std::unique_ptr<Session> session, bool reused)
            : pool_(pool), session_(std::move(session)), reused_(reused) {}
        Pool* pool_ = nullptr;
        std::unique_ptr<Session> session_;
        bool broken_ = false;
        bool reused_ = false;
    };

    // A session: an idle one, else a new login once there is room under the limit and among the
    // logins at once, else waiting for one to come back. nullopt once `stop` is requested. Throws
    // Error: a fatal one at once, a refusal after the retries (`retry_refused`, else the pool's).
    [[nodiscard]] std::optional<Lease> acquire(std::stop_token stop = {}, Retry retry_refused = std::nullopt);

    // Lowering it closes idle connections over it; borrowed ones close when they come back.
    void set_limit(int limit);
    [[nodiscard]] int limit() const;
    // Connections open: idle, borrowed and logging in.
    [[nodiscard]] int open() const;
    [[nodiscard]] int idle() const;
    // Closes connections idle longer than idle_close at `now` (the most recently used one: longer
    // than keep_newest, when that is longer).
    void close_idle(Clock::time_point now);

    // The find check of this pool's server, asked once: `check` runs on the first call (one at a
    // time: a second caller waits for the first one's answer, or until its `stop`, then nullopt)
    // and its answer is kept for find_check_trust, counted from its `checked` when it has one (an
    // answer an earlier run saved), so a program asks each server once; one that isn't lasting
    // only for find_check_expiry. nullopt (stopped) isn't kept; an Error `check` throws goes to
    // the caller and isn't kept either, and a caller waiting runs its own check.
    [[nodiscard]] std::optional<FindCheck> find_check(std::stop_token stop,
                                                      const std::function<std::optional<FindCheck>()>& check);
    // Drops the answer kept, so the next find_check asks again: find failed where it said find
    // works (a saved answer gone stale, ssh::recheck_lookup).
    void forget_find_check();

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
    // How the program's users share their pools: refusals are left to the caller (fetch backs off
    // across all its streams and learns the server's limit, conn_budget; Browse, searches and lists
    // pass interactive_retry); idle connections close after 15 s, as rclone's own folder copies
    // and listings still open theirs beside them (phase 2), but the most recently used one stays
    // for 60 s, so a Browse click or a search a minute later doesn't pay a login (phase 4).
    [[nodiscard]] static Pool::Options download_options(int limit);

    // `limit`: the server's connection budget now.
    [[nodiscard]] std::shared_ptr<Pool> get(const std::string& server, const Target& target, int limit);
    // The server's pool (if any) drops its find check's answer, so its next search loads the one
    // saved since (the window's server Test asked again) instead of keeping the old one for a week.
    void forget_find_check(const std::string& server);

private:
    struct Entry {
        Target target;
        std::shared_ptr<Pool> pool;
    };
    std::mutex mutex_;
    std::map<std::string, Entry> pools_;
};

} // namespace grab::ssh
