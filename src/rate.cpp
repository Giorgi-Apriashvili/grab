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
    const bool tighter = bytes_per_s > 0 && (rate_ == 0 || bytes_per_s < rate_);
    if (bytes_per_s > 0 && rate_ == 0) tokens_ = 0; // switched on: unlimited owes nothing
    rate_ = bytes_per_s;
    tokens_ = std::min(tokens_, tighter ? 0.0 : burst());
    if (tighter) last_.reset(); // fills from the next take or debit
}

TokenBucket::Clock::duration TokenBucket::take(std::uint64_t n, Clock::time_point now) {
    if (rate_ == 0) return Clock::duration::zero();
    refill(now);
    const double need = static_cast<double>(n);
    // A request asked for under a higher rate can exceed the bucket after the rate was lowered:
    // it passes once the bucket is full, leaving it in debt, rather than never.
    if (tokens_ >= need || tokens_ >= burst()) {
        tokens_ -= need;
        return Clock::duration::zero();
    }
    const double seconds = (std::min(need, burst()) - tokens_) / static_cast<double>(rate_);
    return std::max<Clock::duration>(std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds)),
                                     std::chrono::microseconds(100));
}

void TokenBucket::debit(std::uint64_t n, Clock::time_point now) {
    if (rate_ == 0) return;
    refill(now);
    tokens_ -= static_cast<double>(n);
}

// ---- fair queue ------------------------------------------------------------------------------

FairQueue::Ticket FairQueue::add(std::uint64_t flow, std::uint64_t n, double weight, double part) {
    const auto valid = [](double w) { return w > 0 && std::isfinite(w) ? w : 1.0; };
    const bool member = part != 0;
    const std::uint64_t key = member ? flow & ~std::uint64_t{0xFFFFFFFF} : flow; // flow_of(id, 0)
    Download& d = downloads_[key];
    d.weight = valid(weight);
    auto& finish = d.finish_of[flow];
    Ticket t;
    t.start = std::max(d.vtime, finish);
    t.seq = seq_++;
    t.flow = flow;
    t.n = n;
    t.download = key;
    // A download's own flow keeps its weight's tags (the same order, it being alone in it).
    finish = t.start + static_cast<double>(n) / (member ? valid(part) : d.weight);
    t.finish = finish;
    if (d.pending.empty()) { // its turn: where its last one finished, or now if it was idle
        d.start = std::max(vtime_, d.finish);
        d.order = t.seq;
        line_.emplace(d.start, d.order, key);
    }
    d.pending.insert(t);
    return t;
}

const FairQueue::Ticket& FairQueue::head() const {
    return *downloads_.at(std::get<2>(*line_.begin())).pending.begin();
}

void FairQueue::serve(const Ticket& ticket) {
    const Ticket t = ticket; // may be the head itself, gone below
    const auto it = downloads_.find(t.download);
    if (it == downloads_.end() || !it->second.pending.contains(t)) return;
    Download& d = it->second;
    line_.erase({d.start, d.order, t.download});
    d.pending.erase(t);
    vtime_ = std::max(vtime_, d.start);
    d.finish = d.start + static_cast<double>(t.n) / d.weight;
    d.vtime = std::max(d.vtime, t.start);
    std::erase_if(d.finish_of, [&](const auto& f) { return f.second <= d.vtime; });
    if (!d.pending.empty()) { // its next turn
        d.start = d.finish;
        line_.emplace(d.start, d.order, t.download);
    }
    // Downloads with nothing in line that have fallen behind the virtual time carry no state worth
    // keeping.
    std::erase_if(downloads_, [&](const auto& e) { return e.second.pending.empty() && e.second.finish <= vtime_; });
}

void FairQueue::cancel(const Ticket& ticket) {
    const Ticket t = ticket;
    const auto it = downloads_.find(t.download);
    if (it == downloads_.end()) return;
    Download& d = it->second;
    if (d.pending.erase(t) == 0) return;
    if (const auto f = d.finish_of.find(t.flow); f != d.finish_of.end() && f->second == t.finish) f->second = t.start;
    if (d.pending.empty()) line_.erase({d.start, d.order, t.download}); // its turn unused
}

// ---- shared limiter --------------------------------------------------------------------------

void RateLimiter::set_rate(std::uint64_t bytes_per_s) {
    {
        std::lock_guard lock(mutex_);
        const std::uint64_t was = bucket_.rate();
        if (bytes_per_s == was) return;
        bucket_.set_rate(bytes_per_s, Clock::now());
        ++generation_;
        if (bytes_per_s > 0 && (was == 0 || bytes_per_s < was)) {
            // Asked for under the old rate: given up, their streams ask again at the new one (an
            // acquire's at once, a stream's after it looked at what it has on its way). Taking
            // their tokens now would start filling the emptied bucket while dropped streams log
            // in again.
            for (auto it = pending_.rbegin(); it != pending_.rend(); ++it) { // the newest first
                queue_.cancel(it->second.ticket);
                dropped_.insert(it->first);
            }
            pending_.clear();
        } else {
            serve(); // unlimited: everyone in line goes at once; raised: whoever's tokens are there
        }
    }
    changed_.notify_all();
}

std::uint64_t RateLimiter::rate() {
    std::lock_guard lock(mutex_);
    return bucket_.rate();
}

RateLimiter::Setting RateLimiter::setting() {
    std::lock_guard lock(mutex_);
    return Setting{.rate = bucket_.rate(), .generation = generation_};
}

std::uint64_t RateLimiter::generation() {
    std::lock_guard lock(mutex_);
    return generation_;
}

std::optional<RateLimiter::Clock::duration> RateLimiter::serve() {
    const auto now = Clock::now();
    bool served = false;
    std::optional<Clock::duration> wait;
    while (!queue_.empty()) {
        const FairQueue::Ticket head = queue_.head();
        const std::uint64_t id = head.seq + 1;
        const auto w = bucket_.take(pending_.at(id).n, now);
        if (w != Clock::duration::zero()) {
            wait = w;
            break;
        }
        queue_.serve(head);
        pending_.erase(id);
        granted_.insert(id);
        served = true;
    }
    if (served) changed_.notify_all();
    return wait;
}

std::uint64_t RateLimiter::enqueue_locked(std::uint64_t n, std::uint64_t flow, double weight, double part) {
    const auto t = queue_.add(flow, n, weight, part);
    pending_.emplace(t.seq + 1, Pending{.ticket = t, .n = n});
    return t.seq + 1;
}

bool RateLimiter::take_locked(std::uint64_t ticket) { return granted_.erase(ticket) > 0; }

void RateLimiter::cancel_locked(std::uint64_t ticket) {
    if (granted_.erase(ticket) > 0 || dropped_.erase(ticket) > 0) return; // its tokens are gone with it
    const auto it = pending_.find(ticket);
    if (it == pending_.end()) return;
    queue_.cancel(it->second.ticket);
    pending_.erase(it);
    serve(); // the next in line may go now
    changed_.notify_all();
}

RateLimiter::Waited RateLimiter::wait_locked(std::unique_lock<std::mutex>& lock, std::uint64_t ticket,
                                             const std::stop_token& stop, bool until_new_rate) {
    auto seen = generation_;
    for (;;) {
        const auto wait = serve();
        if (take_locked(ticket)) return Waited::served;
        if (dropped_.erase(ticket) > 0) return Waited::renew; // a lower rate came
        if (stop.stop_requested()) {
            cancel_locked(ticket);
            return Waited::stopped;
        }
        if (until_new_rate && generation_ != seen) {
            cancel_locked(ticket);
            return Waited::renew;
        }
        seen = generation_; // acquire's: it goes on waiting at the new rate
        // Wakes when it is served (by whoever looked), when the head's tokens are due (anyone
        // waiting serves it: its stream may be busy reading), for a new rate, or a stop.
        auto done = [&] { return granted_.contains(ticket) || generation_ != seen; };
        if (wait) changed_.wait_for(lock, stop, *wait, done);
        else changed_.wait(lock, stop, done);
    }
}

bool RateLimiter::acquire(std::uint64_t n, std::stop_token stop, std::uint64_t flow, double weight, double part) {
    std::unique_lock lock(mutex_);
    while (n > 0) {
        if (stop.stop_requested()) return false;
        if (bucket_.rate() == 0) return true;
        const auto piece = std::min<std::uint64_t>(n, static_cast<std::uint64_t>(bucket_.burst()));
        // In line by fair-queuing tag; served once at the head with its tokens there. Given up
        // for a lower rate, it asks again at that one.
        const auto waited = wait_locked(lock, enqueue_locked(piece, flow, weight, part), stop, false);
        if (waited == Waited::stopped) return false;
        if (waited == Waited::served) n -= piece;
    }
    return true;
}

void RateLimiter::debit(std::uint64_t n) {
    std::lock_guard lock(mutex_);
    bucket_.debit(n, Clock::now());
}

std::uint64_t RateLimiter::enqueue(std::uint64_t n, std::uint64_t flow, double weight, double part) {
    std::lock_guard lock(mutex_);
    if (bucket_.rate() == 0) return 0;
    const auto ticket = enqueue_locked(n, flow, weight, part);
    serve(); // a bucket a new limit emptied fills from the first request
    return ticket;
}

bool RateLimiter::ready(std::uint64_t ticket) {
    std::lock_guard lock(mutex_);
    serve();
    return take_locked(ticket);
}

bool RateLimiter::wait(std::uint64_t ticket, std::stop_token stop) {
    std::unique_lock lock(mutex_);
    return wait_locked(lock, ticket, stop, true) == Waited::served;
}

void RateLimiter::cancel(std::uint64_t ticket) {
    std::lock_guard lock(mutex_);
    cancel_locked(ticket);
}

} // namespace grab::rate
