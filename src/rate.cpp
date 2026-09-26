#include "rate.hpp"

#include "util.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <system_error>

namespace grab::rate {

std::optional<std::uint64_t> parse_rate(std::string_view text) {
    text = util::trim(text);
    if (text.empty()) return std::nullopt;
    double unit = 1024.0 * 1024.0; // a bare number means MiB/s
    switch (text.back()) {
    case 'k':
    case 'K': unit = 1024.0; text.remove_suffix(1); break;
    case 'm':
    case 'M': unit = 1024.0 * 1024.0; text.remove_suffix(1); break;
    case 'g':
    case 'G': unit = 1024.0 * 1024.0 * 1024.0; text.remove_suffix(1); break;
    default: break;
    }
    double value = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || ptr != text.data() + text.size() || !std::isfinite(value) || value <= 0) {
        return std::nullopt;
    }
    const double bytes = value * unit;
    if (bytes < 1 || bytes > 1e15) return std::nullopt;
    return static_cast<std::uint64_t>(bytes);
}

std::string bwlimit_arg(std::uint64_t bytes_per_s) {
    return std::format("{}K", std::max<std::uint64_t>(1, (bytes_per_s + 1023) / 1024));
}

// ---- token bucket ----------------------------------------------------------------------------

double TokenBucket::burst() const { return std::max(static_cast<double>(rate_) / 4.0, 64.0 * 1024.0); }

void TokenBucket::refill(Clock::time_point now) {
    if (last_ && rate_ > 0) {
        const double elapsed = std::chrono::duration<double>(now - *last_).count();
        if (elapsed > 0) tokens_ = std::min(burst(), tokens_ + elapsed * static_cast<double>(rate_));
    }
    last_ = now;
}

void TokenBucket::set_rate(std::uint64_t bytes_per_s, Clock::time_point now) {
    refill(now);
    rate_ = bytes_per_s;
    tokens_ = std::min(tokens_, burst());
}

TokenBucket::Clock::duration TokenBucket::take(std::uint64_t n, Clock::time_point now) {
    if (rate_ == 0) return Clock::duration::zero();
    refill(now);
    const double need = static_cast<double>(n);
    if (tokens_ >= need) {
        tokens_ -= need;
        return Clock::duration::zero();
    }
    const double seconds = (need - tokens_) / static_cast<double>(rate_);
    return std::max<Clock::duration>(std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds)),
                                     std::chrono::microseconds(100));
}

void TokenBucket::debit(std::uint64_t n, Clock::time_point now) {
    if (rate_ == 0) return;
    refill(now);
    tokens_ -= static_cast<double>(n);
}

// ---- fair queue ------------------------------------------------------------------------------

FairQueue::Ticket FairQueue::add(int flow, std::uint64_t n, int weight) {
    auto& finish = finish_[flow];
    Ticket t;
    t.start = std::max(vtime_, finish);
    t.seq = seq_++;
    t.flow = flow;
    finish = t.start + static_cast<double>(n) / static_cast<double>(std::max(weight, 1));
    pending_.insert(t);
    return t;
}

void FairQueue::serve(const Ticket& t) {
    pending_.erase(t);
    vtime_ = std::max(vtime_, t.start);
    // Flows that have fallen behind the virtual time carry no state worth keeping.
    std::erase_if(finish_, [&](const auto& f) { return f.second <= vtime_; });
}

void FairQueue::cancel(const Ticket& t) { pending_.erase(t); }

// ---- shared limiter --------------------------------------------------------------------------

void RateLimiter::set_rate(std::uint64_t bytes_per_s) {
    {
        std::lock_guard lock(mutex_);
        bucket_.set_rate(bytes_per_s, TokenBucket::Clock::now());
        ++generation_;
    }
    changed_.notify_all();
}

std::uint64_t RateLimiter::rate() {
    std::lock_guard lock(mutex_);
    return bucket_.rate();
}

std::uint64_t RateLimiter::generation() {
    std::lock_guard lock(mutex_);
    return generation_;
}

bool RateLimiter::acquire(std::uint64_t n, std::stop_token stop, int flow, int weight) {
    std::unique_lock lock(mutex_);
    while (n > 0) {
        if (stop.stop_requested()) return false;
        if (bucket_.rate() == 0) return true;
        const auto piece = std::min<std::uint64_t>(n, static_cast<std::uint64_t>(bucket_.burst()));
        // Wait in line by fair-queuing tag; only the head of the line takes tokens.
        const auto ticket = queue_.add(flow, piece, weight);
        for (;;) {
            if (stop.stop_requested() || bucket_.rate() == 0) {
                queue_.cancel(ticket);
                changed_.notify_all(); // the next in line may be the head now
                return !stop.stop_requested();
            }
            if (queue_.head() == ticket) {
                const auto wait = bucket_.take(piece, TokenBucket::Clock::now());
                if (wait == TokenBucket::Clock::duration::zero()) {
                    queue_.serve(ticket);
                    changed_.notify_all();
                    break;
                }
                // Wakes early for a new rate (e.g. the limit switched off) or a stop.
                const auto seen = generation_;
                changed_.wait_for(lock, stop, wait, [&] { return generation_ != seen; });
            } else {
                changed_.wait(lock, stop, [&] { return queue_.head() == ticket || bucket_.rate() == 0; });
            }
        }
        n -= piece;
    }
    return true;
}

void RateLimiter::debit(std::uint64_t n) {
    std::lock_guard lock(mutex_);
    bucket_.debit(n, TokenBucket::Clock::now());
}

} // namespace grab::rate
