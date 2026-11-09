#pragma once

// Download speed limits: a token bucket shared by every stream grab reads, and the rate syntax
// of `grab --limit` / rclone's --bwlimit.

#include <chrono>
#include <condition_variable>
#include <compare> // IWYU pragma: keep (FairQueue::Ticket's defaulted operator<=>)
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>

namespace grab::rate {

// "5M", "800K", "2.5M", "1G" (binary units, as rclone uses them) or a bare number of MiB/s, as
// bytes per second. nullopt for anything else, or below 1 byte/s.
[[nodiscard]] std::optional<std::uint64_t> parse_rate(std::string_view text);

// An rclone --bwlimit value for `bytes_per_s`: whole KiB, at least 1 ("5120K").
[[nodiscard]] std::string bwlimit_arg(std::uint64_t bytes_per_s);

// The bucket itself, with time passed in: pure and testable. Tokens are bytes; the bucket
// holds at most `burst()` of them (a quarter second's worth, at least 64 KiB), and may go
// negative after a debit.
class TokenBucket {
public:
    using Clock = std::chrono::steady_clock;

    // 0 = unlimited. A limit switched on, or lowered, starts with an empty bucket (what came at
    // full speed, or under the higher limit, leaves no credit; a debt stays) that fills again only
    // from the next take or debit: streams dropped for the new limit log in again meanwhile
    // (fetch.cpp), and a bucket filled while they did would let a burst through as they come back.
    void set_rate(std::uint64_t bytes_per_s, Clock::time_point now);
    [[nodiscard]] std::uint64_t rate() const { return rate_; }
    [[nodiscard]] double burst() const;

    // Takes `n` bytes when available and returns zero; otherwise takes nothing and returns how
    // long until they will be. More than burst() (asked for before the rate was lowered) passes
    // once the bucket is full, leaving it in debt.
    [[nodiscard]] Clock::duration take(std::uint64_t n, Clock::time_point now);
    // Bytes that moved without asking (an rclone batch capped by its own --bwlimit).
    void debit(std::uint64_t n, Clock::time_point now);

private:
    void refill(Clock::time_point now);
    std::uint64_t rate_ = 0;
    double tokens_ = 0;
    std::optional<Clock::time_point> last_;
};

// A flow of the limiter: a download (`download`, its id) as a whole is member 0; a folder's files
// are members of their own, which share the folder's turns (FairQueue), so the folder's share
// against other downloads is its weight's however many of its files read. Never the same key for
// two of them.
[[nodiscard]] constexpr std::uint64_t flow_of(int download, std::uint32_t member = 0) {
    return (std::uint64_t{static_cast<std::uint32_t>(download)} << 32) | member;
}

// Weighted fair queuing (start-time fair queuing) in two levels. Downloads are served in order of
// their start tags: a download's next turn starts where its previous one finished, or at the
// current virtual time if it was idle, and finishes n / weight later; so over time each busy
// download gets bytes in proportion to its weight, and an idle one gains no credit to burst with.
// A download's flows (a folder's files: flow_of's members) take its turns the same way among
// themselves, by their parts. So a folder's share is its weight's whatever its files do: files that
// start, end or wait for a connection, and the parts they were asked with, move bytes only among
// them (one level with each file a flow of its part of the folder's weight gave the folder more
// while its files started: tags already handed out kept the larger parts the first files had
// alone). Pure: the caller decides when the head may be served.
class FairQueue {
public:
    struct Ticket {
        double start = 0; // its flow's tag among its download's flows
        std::uint64_t seq = 0;
        std::uint64_t flow = 0;
        double finish = 0; // where its flow's next request starts (seq tells tickets apart first)
        std::uint64_t n = 0;
        std::uint64_t download = 0; // its download's key in the line
        auto operator<=>(const Ticket&) const = default;
    };
    // `n` bytes of `flow`. `weight` > 0 is its download's (high 4, normal 2, low 1; a folder's for
    // its files). `part` > 0 makes the flow one of download flow_of(id, 0)'s flows, taking that part
    // of its turns (a folder's file's priority); none (0) makes it a download of its own. Anything
    // else counts as 1.
    Ticket add(std::uint64_t flow, std::uint64_t n, double weight, double part = 0);
    [[nodiscard]] bool empty() const { return line_.empty(); }
    [[nodiscard]] const Ticket& head() const;
    void serve(const Ticket& t); // takes the head out; virtual time moves to its download's start
    // Takes a request out unserved: its flow's next one starts where it would have, when it was
    // the flow's last, and its download's turn is not used up, so a request given up (a stream
    // stopping, a new limit) costs its flow nothing.
    void cancel(const Ticket& t);

private:
    struct Download {
        double weight = 1;
        double start = 0;        // its turn's tag, while it has requests in line
        double finish = 0;       // where its next turn starts
        std::uint64_t order = 0; // the same starts: who came first
        double vtime = 0;        // its flows' virtual time
        std::map<std::uint64_t, double> finish_of; // per flow
        std::set<Ticket> pending;
    };
    std::map<std::uint64_t, Download> downloads_;
    std::set<std::tuple<double, std::uint64_t, std::uint64_t>> line_; // (start, order, key): those with requests
    double vtime_ = 0;
    std::uint64_t seq_ = 0;
};

// The token bucket for many threads: download streams wait in acquire() until their bytes may
// pass, and competing downloads are served by weight (their bandwidth priority). Unlimited
// (rate 0) passes at once.
//
// A stream that has replies on their way waits for its next READ's tokens without blocking
// (enqueue, then ready after each reply; wait only with nothing on its way), so its replies are
// read as they come rather than after a token turn. Requests are served in one fair order however
// they wait: whoever looks serves every request at the head of the line whose tokens are there,
// so one whose stream is busy reading holds no one up.
class RateLimiter {
public:
    // A new rate; the same one changes nothing (the speed schedule sets it every second).
    void set_rate(std::uint64_t bytes_per_s);
    [[nodiscard]] std::uint64_t rate();
    // The rate and how many times it changed: a stream that sees `generation` change looks at
    // what it has on its way. A limit switched on or lowered below that drops the stream's
    // connection (it logs in again) or charges it to the new rate (fetch.cpp, folder_job.cpp).
    struct Setting {
        std::uint64_t rate = 0;
        std::uint64_t generation = 0;
    };
    [[nodiscard]] Setting setting();
    [[nodiscard]] std::uint64_t generation();
    // Waits until `n` bytes may pass; false when `stop` was requested first. `flow` identifies
    // the download or a folder's file (flow_of; the streams of one share it), `weight` the
    // download's priority (high 4, normal 2, low 1; a folder's for its files) and `part` a folder's
    // file's part of the folder's turns (its priority; 0 for a download as a whole: FairQueue::add).
    bool acquire(std::uint64_t n, std::stop_token stop, std::uint64_t flow = 0, double weight = 2, double part = 0);
    // Bytes that come without having taken tokens: what a stream still had on its way when the
    // limit was lowered.
    void debit(std::uint64_t n);

    // `n` bytes (at most burst-sized, a READ's) in line as acquire's, without waiting: a ticket,
    // or 0 when unlimited (nothing to wait for).
    [[nodiscard]] std::uint64_t enqueue(std::uint64_t n, std::uint64_t flow, double weight, double part = 0);
    // Whether the ticket's bytes may pass now; then it is spent. Never waits.
    [[nodiscard]] bool ready(std::uint64_t ticket);
    // Waits until they may (true, spent), or until `stop` or a new rate (false: the ticket is
    // given up, and the caller looks again at the new rate).
    bool wait(std::uint64_t ticket, std::stop_token stop);
    // Gives a ticket up, spent or not (a stream stopping, or looking again at a new rate).
    void cancel(std::uint64_t ticket);

private:
    using Clock = TokenBucket::Clock;
    struct Pending {
        FairQueue::Ticket ticket;
        std::uint64_t n = 0;
    };
    // Under mutex_. Serves the head of the line while its tokens are there; how long until the
    // next one's are, or nullopt with no one in line.
    std::optional<Clock::duration> serve();
    std::uint64_t enqueue_locked(std::uint64_t n, std::uint64_t flow, double weight, double part);
    bool take_locked(std::uint64_t ticket);   // spent: true, and forgotten
    void cancel_locked(std::uint64_t ticket);
    // renew: a lower rate gave the ticket up, or (`until_new_rate`) any new rate came.
    enum class Waited { served, stopped, renew };
    Waited wait_locked(std::unique_lock<std::mutex>& lock, std::uint64_t ticket, const std::stop_token& stop,
                       bool until_new_rate);

    std::mutex mutex_;
    std::condition_variable_any changed_;
    TokenBucket bucket_;
    FairQueue queue_;
    std::map<std::uint64_t, Pending> pending_; // by ticket (FairQueue seq + 1)
    std::set<std::uint64_t> granted_;          // served, not yet taken by their streams
    std::set<std::uint64_t> dropped_;          // given up for a lower rate, not yet told
    std::uint64_t generation_ = 0;             // bumped by set_rate, so waiters recompute at once
};

// A stream's next request in a limiter's line (RateLimiter::enqueue), given up as the stream goes.
struct Place {
    RateLimiter* limiter = nullptr;
    std::uint64_t ticket = 0; // 0: none
    std::uint64_t n = 0;      // its bytes
    explicit Place(RateLimiter* l) : limiter(l) {}
    Place(const Place&) = delete;
    Place& operator=(const Place&) = delete;
    ~Place() { drop(); }
    void drop() {
        if (ticket != 0) limiter->cancel(ticket);
        ticket = 0;
    }
};

} // namespace grab::rate
