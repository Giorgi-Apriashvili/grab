#pragma once

// A download's speed and time left as the window shows them (DOWNLOADS-UI.md, chunk G), from the
// bytes that actually came: each READ's reply is counted as it arrives, a folder's small files'
// and big files' alike, rather than added up from the files' own speeds (small files have none:
// they land too quickly), so a folder of small files has a speed and a time left too. Pure, with
// the time passed in, so it is unit-tested; grab-gui samples its counters at each 250 ms flush.

#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>

namespace grab::speed {

using Clock = std::chrono::steady_clock;

// The speed is a recent average: what came over about the last 5 s. What came up to meter_full ago
// counts fully, older bytes less and less until meter_gone, so a burst leaving the average fades
// out over two seconds instead of stepping down at once.
inline constexpr std::chrono::milliseconds meter_full{4000};
inline constexpr std::chrono::milliseconds meter_gone{6000};
// No speed until the samples with bytes span this long.
inline constexpr std::chrono::milliseconds meter_least{500};
// For meter_full after a restart (Meter::restart), the speed is what came over about the last this
// long, so it follows the new limit's first seconds closely.
inline constexpr std::chrono::milliseconds meter_settle{1000};

// The speed of a counter of bytes that came (it only goes up), sampled now and then. It starts at
// the first bytes, and again once its window holds none: the time spent connecting, or paused,
// doesn't water down the speed the bytes then come at.
class Meter {
public:
    // The counter now. The first sample only starts the meter.
    void sample(Clock::time_point now, std::uint64_t total);
    // A new speed limit applies (chunk J): the window starts again at `now` (the counter then
    // `total`), so the new speed shows once the bytes after it span meter_least, about a second
    // later, rather than fading in over meter_gone. Until then rate() stays what it was (no 0 in
    // between), as it does while its newest sample brought nothing, and since() stays (the time
    // left goes on). Half a second without bytes meanwhile starts it again at the next ones:
    // streams dropped for the new limit log in again, and what came just before they dropped is
    // not the new speed. Once it shows, until meter_full after the restart, it is the speed of
    // about the last meter_settle (0 when nothing came in it): what came at first (replies already
    // on their way, connections catching up after logging in again) leaves it within a second
    // rather than weighing on it for five. Its usual rules again after that; if it never showed
    // and no bytes came meanwhile, it starts again at the next ones (rate 0, no since()).
    // `limit`: the new limit in bytes per second (0: none), for ahead().
    void restart(Clock::time_point now, std::uint64_t total, double limit = 0);
    // Bytes per second, as of the last sample; 0 before meter_least of samples with bytes.
    [[nodiscard]] double rate() const { return rate_; }
    // Restarted, and the speed after it doesn't show yet: rate() is still the one from before.
    [[nodiscard]] bool holding() const { return holding_; }
    // Restarted for a limit below the speed then: the bytes come at that limit from now on (the
    // link gave more), which is known at once, while the first speed measured after the change is
    // off by up to a third (replies already on their way when it came, then the limiter's pause
    // for them). Until meter_full after the restart.
    [[nodiscard]] bool foreseen() const { return limited_ > 0; }
    // The speed for the time left: the limit while foreseen(), rate() otherwise.
    [[nodiscard]] double ahead() const { return foreseen() ? limited_ : rate_; }
    // When the bytes it measures started coming (the sample before the first of them); nullopt
    // while none came.
    [[nodiscard]] std::optional<Clock::time_point> since() const { return since_; }

private:
    struct Step {
        Clock::time_point end;
        Clock::duration took{};
        std::uint64_t bytes = 0;
    };
    std::deque<Step> steps_;
    std::optional<Clock::time_point> at_;
    std::optional<Clock::time_point> since_;
    std::optional<Clock::time_point> restarted_; // then, up to meter_full ago
    bool holding_ = false;                       // restarted, and the old rate shown until the new one shows
    std::uint64_t total_ = 0;
    double rate_ = 0;
    double limited_ = 0; // foreseen(): the limit, bytes per second
};

// A download's pace, for its time left, learned from its own recent progress in both files and
// bytes:
//
//     time left ≈ per_file × files left + per_byte × bytes left
//
// per_file is what each file costs besides its bytes (its round trips: opening, the size check,
// closing), which sets the pace of a folder of many small files; per_byte is 1 / the bandwidth.
// Fitted over the last pace_window of fetching, a point every pace_step: the model gives the
// window's own time exactly (per_file × its files + per_byte × its bytes = its seconds), and how
// that time splits between the files and the bytes (θ, the files' share) comes from how the mix
// of files and bytes varied from one second to the next, by least squares:
//
//     u_k = T × files_k / F,  v_k = T × bytes_k / B   (second k if all time were files', or bytes')
//     θ = Σ (t_k − v_k)(u_k − v_k) / (Σ (u_k − v_k)² + pace_ridge × Σ t_k²),  kept within [0, 1]
//     per_file = θ × T / F,  per_byte = (1 − θ) × T / B
//
// When the seconds can't tell the two apart (files of much the same size), the ridge gives the
// time to the bytes: the files left then cost what bytes cost now. And per_byte is at least what
// the window's fastest second's bytes cost (per_file then takes what is left of the time, never
// below 0), so the bytes of a folder's big files that wait behind its small ones never come out
// cheaper than the most bytes a second has brought: it errs long, never short. With no files
// finished in the window (a single file, or a folder's big file alone) it is bytes alone:
// per_byte = the window's seconds / its bytes.
//
// The window's costs are its 20 s average; time_left scales them by the speed now (the Meter's
// ~5 s) against that average, so a change in the link's speed shows within seconds. A new speed
// limit starts the window again (restart): seconds at the old speed and the new one in one window
// make a pace neither has, and the time left fell far too fast for the window's 20 s.
inline constexpr std::chrono::seconds pace_window{20};
inline constexpr std::chrono::seconds pace_step{1};
// No time left until the points span this long ("blank until it has enough data").
inline constexpr std::chrono::seconds pace_least{5};
inline constexpr double pace_ridge = 0.001;

class Pace {
public:
    // Files finished so far and bytes come so far (counters; a file queued again may take one
    // back, which counts as none). A point is kept at most every pace_step.
    void sample(Clock::time_point now, std::uint64_t files, std::uint64_t bytes);
    // A new speed limit applies (with Meter::restart): the points so far go, the next sample
    // starts the window again (sample it once the new speed shows, Meter::holding). Until its
    // points span pace_least again, time_left with a rate is the bytes left at that rate, when
    // it had a pace before (so a time left shown before goes on); nullopt as at the start if not.
    void restart();
    struct Model {
        double per_file = 0; // seconds
        double per_byte = 0; // seconds
        double rate = 0;     // the window's bytes per second
    };
    // nullopt until the points span pace_least, or when nothing came in the window.
    [[nodiscard]] std::optional<Model> model() const;
    // The time left for `files` files and `bytes` bytes, in seconds, by model().
    [[nodiscard]] std::optional<double> time_left(std::uint64_t files, std::uint64_t bytes) const;
    // The same, its costs scaled by `rate` (bytes per second now) against the window's: the pace
    // as it is now. nullopt also while nothing comes now (rate 0). After restart(), see there.
    [[nodiscard]] std::optional<double> time_left(std::uint64_t files, std::uint64_t bytes, double rate) const;

private:
    struct Point {
        Clock::time_point at;
        std::uint64_t files = 0;
        std::uint64_t bytes = 0;
    };
    std::deque<Point> points_;
    bool restarted_ = false; // restarted with a pace, and the window not pace_least long again yet
};

// The time left as the row shows it: it counts down between estimates and moves part of the way
// towards each new one (most of the way within about two countdown_tau), so a noisy estimate
// doesn't make it jump. It stays within a factor countdown_band of the estimate, so it can't count
// down to 0 while the estimate says seconds are left (the tail of a file, slower than its speed).
inline constexpr std::chrono::seconds countdown_tau{3};
inline constexpr double countdown_band = 2;

class Countdown {
public:
    // The estimate now (nullopt: none, which blanks it); the time left to show.
    std::optional<double> next(Clock::time_point now, std::optional<double> estimate);
    // Instead of next() while a new speed limit's speed doesn't show yet (Meter::holding): the
    // estimates still come from the old speed, so what it shows only counts down (nullopt if
    // nothing). The next estimate after it is shown as it is: the time left of the new speed at
    // once, not approached over seconds (it would be off by up to half for about five).
    std::optional<double> hold(Clock::time_point now);
    // A new speed limit whose speed is known at once (Meter::foreseen): the next estimate is shown
    // as it is.
    void restart() { jump_ = true; }

private:
    std::optional<double> shown_;
    Clock::time_point at_{};
    bool jump_ = false; // held: the next estimate is shown as it is
};

// A single file's time left: its bytes left at its speed, once its bytes have come for pace_least
// (`since`, Meter::since): its first seconds' speed is too unsure. nullopt before, or with no
// speed or nothing left.
[[nodiscard]] std::optional<double> file_left(std::optional<Clock::time_point> since, Clock::time_point now,
                                              std::uint64_t bytes_left, double rate);

// A speed as the window writes it (the page's fmtSize: whole bytes below 1 KiB, else one decimal
// of KiB, MiB, GiB, ...), in bytes per second, so a change too small to show isn't sent.
[[nodiscard]] std::uint64_t shown(double bytes_per_s);

} // namespace grab::speed
