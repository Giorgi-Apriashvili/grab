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

#include "conn_budget.hpp"
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
#include <utility>
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
    // Its SFTP session, started on the first call (open_session). Throws sftp::Broken when it can't
    // start, as when the link breaks later: a borrower drops the lease and, if it was reused, may
    // try another.
    [[nodiscard]] virtual sftp::Client& sftp() = 0;
    // Runs a command (find, md5sum) on this login: Connection::exec, or canned output in tests.
    // Where there is none, the server refuses it.
    [[nodiscard]] virtual std::expected<ExecResult, ExecError> exec(std::string_view command,
                                                                    const ExecOptions& options);
    // The login itself; nullptr where there is none (tests).
    [[nodiscard]] virtual Connection* connection() { return nullptr; }
    // Connection::keepalive, for a borrower keeping it idle (Play through a pause); true where
    // there is no login. false: the link failed, drop the lease as broken.
    virtual bool keepalive() { return connection() == nullptr || connection()->keepalive(); }
    // Whether the next borrower may have it: no SFTP reply still on its way, which would reach
    // a request the next one never made, no command cut short on it (Connection::dropped), and
    // no SFTP that failed to start.
    [[nodiscard]] virtual bool reusable() { return true; }
};

// Logs in to `target`; SFTP starts when the session's sftp() is first called, so a login used only
// for commands (find) never pays for it. Throws Error.
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

// Why grab's verification has no server sum for a file (ssh_hash.hpp). None of them says the file
// differs: a check that can't run keeps the copy.
enum class HashFailure {
    no_checksums, // the server can't compute them: no md5sum, a Windows shell, an SFTP-only account, a banner
    unsupported,  // grab can't ask for this one: rclone.conf's keys, a name the server's shell can't be given
    login,        // no login to the server
    refused,      // the server refused the command, also on a login with nothing else open
    deadline,     // the server took too long
    stopped,      // the stop token
    lost,         // the connection broke, or md5sum ended in a way that says nothing (a signal, too much output)
    unpaired,     // md5sum ran, but no record named the file: missing or unreadable there, or a record grab can't read
};

// What a server answered grab's hash check (ssh_hash.hpp): whether its md5sum works as grab needs
// it, and the folder its paths start from.
struct HashCheck {
    bool works = false;
    bool z = false;     // with -z: each record ends in a NUL and names the file exactly as it was given
    std::string home;   // the login folder (SFTP REALPATH of "."), raw bytes: files go to md5sum absolute from it
    std::string answer; // what it said, in a few words, for -v and "not verified"
    HashFailure failure = HashFailure::no_checksums; // why not, when it doesn't work
    // false: it may pass (no answer in time, a lost connection, a login that failed for now), so it
    // holds only find_check_fresh; else for the pool's life. A refusal (a channel refused even on
    // a new login) is never lasting.
    bool lasting = true;
    bool operator==(const HashCheck&) const = default;
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
        // How long find_check keeps an answer that isn't lasting, and one that is. hash_check keeps
        // one that isn't lasting as long, one that is for the pool's life.
        std::chrono::milliseconds find_check_expiry{ssh::find_check_fresh};
        std::chrono::milliseconds find_check_trust{ssh::find_check_trust};
        int hashes_at_once = 1; // hash commands on this server at once (hash_turn)
        // How long acquire_remembered keeps a login failure that may pass by itself (no answer in
        // time, the server turned it away after the retries).
        std::chrono::milliseconds login_failure_holds{30'000};
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
    // `logins_at_once`: this caller's own cap on the pool's logins in progress (Play's burst on a
    // Storage Box, which accepted 8 at once); 0: Options::logins_at_once. Other callers keep theirs.
    [[nodiscard]] std::optional<Lease> acquire(std::stop_token stop = {}, Retry retry_refused = std::nullopt,
                                               int logins_at_once = 0);
    // A new login, never an idle connection, for a command the server refused beside the SFTP
    // session of a reused one (MaxSessions 1): nothing else runs on it. When the pool is full, its
    // oldest idle connection is closed to make room (idle ones count toward the limit); else as
    // acquire.
    [[nodiscard]] std::optional<Lease> acquire_new(std::stop_token stop = {}, Retry retry_refused = std::nullopt);
    // As acquire, for the window's previews and Play (ssh_read.hpp), which must not log in again
    // and again where logins fail: flipping through 10 images would make 20-30 failed logins in
    // seconds, a fail2ban ban on Main that stops rclone and the downloads too. A login that failed
    // here is thrown again without logging in: one that can't pass by itself (Failure::fatal: a
    // host key not trusted, a login refused) until forget_login_failure, another for
    // Options::login_failure_holds. An idle connection is still handed out, and any caller's login
    // that works (downloads, Browse) clears it. Until a login worked, these log in one at a time,
    // the others waiting for its outcome. `logins_at_once` as acquire's.
    [[nodiscard]] std::optional<Lease> acquire_remembered(std::stop_token stop = {},
                                                          Retry retry_refused = std::nullopt, int logins_at_once = 0);
    // The next acquire_remembered logs in again: a Test that worked, Trust, a server saved.
    void forget_login_failure();

    // A turn to run a hash command (ssh_hash.hpp), held while it runs: at most
    // Options::hashes_at_once of them at once on this server, however many verifications share the
    // pool. nullopt once `stop` is requested.
    class Turn {
    public:
        Turn(Turn&& other) noexcept : pool_(std::exchange(other.pool_, nullptr)) {}
        Turn& operator=(Turn&& other) noexcept;
        ~Turn() { release(); }
        Turn(const Turn&) = delete;
        Turn& operator=(const Turn&) = delete;

    private:
        friend class Pool;
        explicit Turn(Pool* pool) : pool_(pool) {}
        void release();
        Pool* pool_ = nullptr;
    };
    [[nodiscard]] std::optional<Turn> hash_turn(std::stop_token stop = {});
    [[nodiscard]] int hashes_at_once() const;

    // Lowering it closes idle connections over it; borrowed ones close when they come back.
    void set_limit(int limit);
    [[nodiscard]] int limit() const;
    // Connections open: idle, borrowed and logging in.
    [[nodiscard]] int open() const;
    [[nodiscard]] int idle() const;
    // Closes connections idle longer than idle_close at `now` (the most recently used one: longer
    // than keep_newest, when that is longer).
    void close_idle(Clock::time_point now);
    // Closes idle connections but the `keep` most recently used, however long they sat: for an
    // engine = rclone server, whose rclone can't see them, once the seats that counted them went
    // back (the window's viewer).
    void trim_idle(int keep);

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

    // The hash check of this pool's server (ssh::hash_check), asked as find_check is: once, a second
    // caller waiting for the first one's answer. An answer that is lasting holds for the pool's life
    // (it isn't saved across runs), one that isn't for find_check_expiry. A refusal is one that isn't
    // lasting: the check already asked again on a new login with nothing else open (a server that
    // allows one channel per login refuses a command beside a busy SFTP session), so asking for
    // every verification would only cost a login each time.
    [[nodiscard]] std::optional<HashCheck> hash_check(std::stop_token stop,
                                                      const std::function<std::optional<HashCheck>()>& check);
    // Keeps `answer` in place of the check's (a login that failed after it: later verifications get
    // its answer instead of trying again), or drops it (nullopt).
    void set_hash_check(std::optional<HashCheck> answer);

private:
    struct Idle {
        std::unique_ptr<Session> session;
        Clock::time_point since;
    };
    // A question about the server the pool asks once (find_check, hash_check).
    template <class Check>
    struct Asked {
        bool asking = false; // a caller is asking; the others wait for its answer
        std::optional<Check> answer;
        Clock::time_point since; // when the answer was given
    };
    template <class Check>
    [[nodiscard]] std::optional<Check> ask(Asked<Check>& asked, const std::stop_token& stop,
                                           const std::function<std::optional<Check>()>& check);
    // How long an answer holds, from when it was given.
    [[nodiscard]] Clock::duration holds(const FindCheck& check) const;
    [[nodiscard]] Clock::duration holds(const HashCheck& check) const;
    // A new login under the lock, which it gives up while logging in.
    [[nodiscard]] std::optional<Lease> log_in(std::unique_lock<std::mutex>& lock, const std::stop_token& stop,
                                              std::span<const std::chrono::milliseconds> pauses);
    void give_back(std::unique_ptr<Session> session, bool broken);
    [[nodiscard]] int open_locked() const { return leased_ + logging_in_ + static_cast<int>(idle_.size()); }

    Connect connect_;
    Options options_;
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::deque<Idle> idle_; // most recently used at the back
    int leased_ = 0;
    int logging_in_ = 0;
    int hashing_ = 0; // hash turns out
    Asked<FindCheck> find_asked_;
    Asked<HashCheck> hash_asked_;
    // acquire_remembered's: the login that failed, whether one of its logins worked since, and
    // whether one is logging in while none has.
    struct LoginFailure {
        budget::Failure kind = budget::Failure::other;
        std::string what;
        Clock::time_point since;
    };
    std::optional<LoginFailure> login_failure_;
    bool login_works_ = false;
    bool login_trying_ = false;
    std::jthread timer_;
};

// Hash commands at once on a server (Pool::Options::hashes_at_once), as phase 5 measured them:
// 2 on a Storage Box (*.your-storagebox.de; 475 MiB/s with one, 900 with two, 953 with four),
// 1 elsewhere (Main's disks: 198 MiB/s with one, 167 with two, 158 with four).
[[nodiscard]] int hashes_at_once_for(std::string_view host);

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
    // The server's pool (if any) drops its hash check's answer, so its next verification asks
    // again: a login that failed for good (a host key not trusted yet, an agent without the key)
    // may work after Trust or a server Test.
    void forget_hash_check(const std::string& server);
    // The server's pool (if any) forgets the login that failed for previews and Play
    // (Pool::acquire_remembered): after a Test that logged in, Trust, or the server saved.
    void forget_login_failure(const std::string& server);
    // A server removed (or changed to another address): its pool's idle connections close, its
    // find and hash answers and a failed login go, and it leaves the program's pools, so that a
    // server added under the name later gets a pool of its own. Downloads still on it keep it
    // until they end.
    void forget(const std::string& server);

private:
    struct Entry {
        Target target;
        std::shared_ptr<Pool> pool;
    };
    std::mutex mutex_;
    std::map<std::string, Entry> pools_;
};

} // namespace grab::ssh
