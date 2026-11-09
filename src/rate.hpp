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
// are members of their own, whose weights add up to the folder's (DOWNLOADS-UI.md 7), so the
// folder's share against other downloads stays the same. Never the same key for two of them.
[[nodiscard]] constexpr std::uint64_t flow_of(int download, std::uint32_t member = 0) {
    return (std::uint64_t{static_cast<std::uint32_t>(download)} << 32) | member;
}

// Weighted fair queuing (start-time fair queuing): requests from many flows (downloads, or a
// folder's files) are served in order of their start tags. A flow's next request starts where its
// previous one finished, or at the current virtual time if it was idle, and finishes n / weight
// later; so over time each busy flow gets bytes in proportion to its weight, and an idle flow
// gains no credit to burst with. Pure: the caller decides when the head may be served.
class FairQueue {
public:
    struct Ticket {
        double start = 0;
        std::uint64_t seq = 0;
        std::uint64_t flow = 0;
        double finish = 0; // where its flow's next request starts (seq tells tickets apart first)
        auto operator<=>(const Ticket&) const = default;
    };
    // `weight` > 0; a folder's file has a part of its folder's (2 × 1 / 7, say). Anything else
    // counts as 1.
    Ticket add(std::uint64_t flow, std::uint64_t n, double weight);
    [[nodiscard]] bool empty() const { return pending_.empty(); }
    [[nodiscard]] const Ticket& head() const { return *pending_.begin(); }
    void serve(const Ticket& t); // takes the head out; virtual time moves to its start
    // Takes a request out unserved: its flow's next one starts where it would have, when it was
    // the flow's last, so a request given up (a stream stopping, a new limit) costs its flow nothing.
    void cancel(const Ticket& t);

private:
    std::set<Ticket> pending_;
    std::map<std::uint64_t, double> finish_; // per flow
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
    // the download or a folder's file (flow_of; the streams of one share it) and `weight` its
    // priority (high 4, normal 2, low 1), or a folder's file's part of its folder's.
    bool acquire(std::uint64_t n, std::stop_token stop, std::uint64_t flow = 0, double weight = 2);
    // Bytes that come without having taken tokens: what a stream still had on its way when the
    // limit was lowered.
    void debit(std::uint64_t n);

    // `n` bytes (at most burst-sized, a READ's) in line as acquire's, without waiting: a ticket,
    // or 0 when unlimited (nothing to wait for).
    [[nodiscard]] std::uint64_t enqueue(std::uint64_t n, std::uint64_t flow, double weight);
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
    std::uint64_t enqueue_locked(std::uint64_t n, std::uint64_t flow, double weight);
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
