#pragma once

// Download speed limits: a token bucket shared by every stream grab-gui reads, and the rate
// syntax of `grab --limit` / rclone's --bwlimit.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
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

// The token bucket for many threads: download streams wait in acquire() until their bytes may
// pass. Unlimited (rate 0) passes at once.
class RateLimiter {
public:
    void set_rate(std::uint64_t bytes_per_s);
    [[nodiscard]] std::uint64_t rate();
    // Changes with every set_rate: streams started under another setting restart to pick up
    // the right read-ahead (see fetch.cpp).
    [[nodiscard]] std::uint64_t generation();
    // Waits until `n` bytes may pass; false when `stop` was requested first.
    bool acquire(std::uint64_t n, std::stop_token stop);
    void debit(std::uint64_t n);

private:
    std::mutex mutex_;
    std::condition_variable_any changed_;
    TokenBucket bucket_;
    std::uint64_t generation_ = 0; // bumped by set_rate, so waiters recompute at once
};

} // namespace grab::rate
