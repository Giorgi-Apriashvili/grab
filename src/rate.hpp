#pragma once

// Download speed limits: a token bucket shared by every stream grab-gui reads, and the rate
// syntax of `grab --limit` / rclone's --bwlimit.

#include <chrono>
#include <condition_variable>
#include <compare>
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

    // 0 = unlimited.
    void set_rate(std::uint64_t bytes_per_s, Clock::time_point now);
    [[nodiscard]] std::uint64_t rate() const { return rate_; }
    [[nodiscard]] double burst() const;

    // Takes `n` bytes (n <= burst()) when available and returns zero; otherwise takes nothing
    // and returns how long until they will be.
    [[nodiscard]] Clock::duration take(std::uint64_t n, Clock::time_point now);
    // Bytes that moved without asking (an rclone batch capped by its own --bwlimit).
    void debit(std::uint64_t n, Clock::time_point now);

private:
    void refill(Clock::time_point now);
    std::uint64_t rate_ = 0;
    double tokens_ = 0;
    std::optional<Clock::time_point> last_;
};

// Weighted fair queuing (start-time fair queuing): requests from many flows (downloads) are
// served in order of their start tags. A flow's next request starts where its previous one
// finished, or at the current virtual time if it was idle, and finishes n / weight later; so
// over time each busy flow gets bytes in proportion to its weight, and an idle flow gains no
// credit to burst with. Pure: the caller decides when the head may be served.
class FairQueue {
public:
    struct Ticket {
        double start = 0;
        std::uint64_t seq = 0;
        int flow = 0;
        auto operator<=>(const Ticket&) const = default;
    };
    Ticket add(int flow, std::uint64_t n, int weight);
    [[nodiscard]] bool empty() const { return pending_.empty(); }
    [[nodiscard]] const Ticket& head() const { return *pending_.begin(); }
    void serve(const Ticket& t); // takes the head out; virtual time moves to its start
    void cancel(const Ticket& t);

private:
    std::set<Ticket> pending_;
    std::map<int, double> finish_; // per flow
    double vtime_ = 0;
    std::uint64_t seq_ = 0;
};

// The token bucket for many threads: download streams wait in acquire() until their bytes may
// pass, and competing downloads are served by weight (their bandwidth priority). Unlimited
// (rate 0) passes at once.
class RateLimiter {
public:
    void set_rate(std::uint64_t bytes_per_s);
    [[nodiscard]] std::uint64_t rate();
    // Changes with every set_rate: streams started under another setting restart to pick up
    // the right read-ahead (see fetch.cpp).
    [[nodiscard]] std::uint64_t generation();
    // Waits until `n` bytes may pass; false when `stop` was requested first. `flow` identifies
    // the download (its streams share one flow) and `weight` its priority (high 4, normal 2,
    // low 1).
    bool acquire(std::uint64_t n, std::stop_token stop, int flow = 0, int weight = 2);
    void debit(std::uint64_t n);

private:
    std::mutex mutex_;
    std::condition_variable_any changed_;
    TokenBucket bucket_;
    FairQueue queue_;
    std::uint64_t generation_ = 0; // bumped by set_rate, so waiters recompute at once
};

} // namespace grab::rate
