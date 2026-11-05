#include "speed.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

using namespace grab;
using namespace std::chrono_literals;
using speed::Clock;

namespace {

constexpr double KiB = 1024;
constexpr double MiB = 1024 * 1024;

// A made-up history's clock: the flush's 250 ms ticks from an arbitrary start.
const Clock::time_point t0 = Clock::time_point{} + std::chrono::hours(1000);
Clock::time_point at(double seconds) {
    return t0 + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
}
// Tick k's time: k quarter seconds.
double tick(int k) { return k * 0.25; }

// Pseudo-random numbers in [0, 1), the same every run.
struct Noise {
    std::uint64_t s = 0x9E3779B97F4A7C15ULL;
    double next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return static_cast<double>(s >> 11) / 9007199254740992.0;
    }
};

} // namespace

TEST_CASE("speed: what came over about the last 5 s, from a counter of bytes") {
    speed::Meter m;
    double bytes = 0;
    double t = 0;
    auto run = [&](double rate, double seconds) {
        for (double end = t + seconds; t < end - 1e-9;) {
            t += 0.25;
            bytes += rate * 0.25;
            m.sample(at(t), static_cast<std::uint64_t>(bytes));
        }
    };
    m.sample(at(0), 0);
    run(MiB, 0.25);
    CHECK(m.rate() == 0); // less than half a second: no speed yet
    run(MiB, 0.25);
    CHECK(m.rate() == doctest::Approx(MiB).epsilon(0.001));
    run(MiB, 9);
    CHECK(m.rate() == doctest::Approx(MiB).epsilon(0.001));
    // Three times as fast: followed within a few seconds, all of it once 6 s went by.
    run(3 * MiB, 2);
    const double after2 = m.rate();
    CHECK(after2 > 1.3 * MiB);
    CHECK(after2 < 2.0 * MiB);
    run(3 * MiB, 2);
    CHECK(m.rate() == doctest::Approx(2.6 * MiB).epsilon(0.001)); // 4 s at 3, 2 s at 1 fading
    run(3 * MiB, 2.25);
    CHECK(m.rate() == doctest::Approx(3 * MiB).epsilon(0.001));
    // Nothing comes: it fades to 0 within 6 s, and is 0 then.
    run(0, 3);
    CHECK(m.rate() > 0);
    run(0, 3.25);
    CHECK(m.rate() == 0);
}

TEST_CASE("speed: a burst leaving the average fades out over two seconds instead of stepping down") {
    speed::Meter m;
    m.sample(at(0), 0);
    std::uint64_t bytes = 0;
    double t = 0;
    for (int k = 0; k < 40; ++k) { // 10 s at 1 MiB/s
        t += 0.25;
        bytes += static_cast<std::uint64_t>(MiB / 4);
        m.sample(at(t), bytes);
    }
    t += 0.25;
    bytes += static_cast<std::uint64_t>(4 * MiB); // a quarter second's burst: 4 MiB more
    m.sample(at(t), bytes);
    std::vector<double> rates;
    for (int k = 0; k < 40; ++k) {
        t += 0.25;
        bytes += static_cast<std::uint64_t>(MiB / 4);
        m.sample(at(t), bytes);
        rates.push_back(m.rate());
    }
    double drop = 0; // the largest fall from one tick to the next
    for (std::size_t k = 1; k < rates.size(); ++k) drop = std::max(drop, rates[k - 1] - rates[k]);
    // A 5 s box would lose the burst's 4 MiB / 5 s = 0.8 MiB/s at once.
    MESSAGE("largest fall per tick: ", drop / MiB, " MiB/s");
    CHECK(drop < 0.3 * MiB);
    CHECK(rates.back() == doctest::Approx(MiB).epsilon(0.001));
}

TEST_CASE("speed: a counter that goes back counts nothing, and samples out of order are dropped") {
    speed::Meter m;
    m.sample(at(0), 1000);
    m.sample(at(1), 2000);
    CHECK(m.rate() == doctest::Approx(1000));
    m.sample(at(2), 500); // went back: nothing came
    CHECK(m.rate() == doctest::Approx(500));
    m.sample(at(1.5), 9000); // earlier than the last: dropped
    CHECK(m.rate() == doctest::Approx(500));
    m.sample(at(3), 1500);
    CHECK(m.rate() == doctest::Approx(1000 * 2.0 / 3.0));
}

TEST_CASE("speed: it starts at the first bytes, not at the time spent connecting, and again after a pause") {
    speed::Meter m;
    double t = 0;
    std::uint64_t bytes = 0;
    auto run = [&](double rate, double seconds) {
        for (double end = t + seconds; t < end - 1e-9;) {
            t += 0.25;
            bytes += static_cast<std::uint64_t>(rate * 0.25);
            m.sample(at(t), bytes);
        }
    };
    m.sample(at(0), 0);
    run(0, 2); // connecting
    CHECK_FALSE(m.since().has_value());
    run(4 * MiB, 1);
    CHECK(m.since().value_or(at(-1)) == at(2)); // the sample before its first bytes
    CHECK(m.rate() == doctest::Approx(4 * MiB).epsilon(0.001)); // not watered down by the 2 s before
    run(4 * MiB, 4);
    CHECK(m.rate() == doctest::Approx(4 * MiB).epsilon(0.001));
    // Paused: it fades to 0 and holds nothing; when the bytes come again it starts from them.
    run(0, 10);
    CHECK(m.rate() == 0);
    CHECK_FALSE(m.since().has_value());
    run(2 * MiB, 1);
    CHECK(m.since().value_or(at(-1)) == at(17));
    CHECK(m.rate() == doctest::Approx(2 * MiB).epsilon(0.001));
}

TEST_CASE("speed: shown as the window writes it, one decimal of its unit") {
    CHECK(speed::shown(0) == 0);
    CHECK(speed::shown(-5) == 0);
    CHECK(speed::shown(std::nan("")) == 0);
    CHECK(speed::shown(512.4) == 512);
    CHECK(speed::shown(1536) == 1536);                                    // 1.5 KiB
    CHECK(speed::shown(12.34 * MiB) == static_cast<std::uint64_t>(std::llround(12.3 * MiB)));
    CHECK(speed::shown(12.36 * MiB) == static_cast<std::uint64_t>(std::llround(12.4 * MiB)));
    CHECK(speed::shown(1048570) == 1048576);                              // 1024.0 KiB: 1.0 MiB
    CHECK(speed::shown(2.5 * 1024 * MiB) == static_cast<std::uint64_t>(2.5 * 1024 * MiB));
    // Values the window writes the same are the same.
    CHECK(speed::shown(12.31 * MiB) == speed::shown(12.33 * MiB));
}

TEST_CASE("time left: no files finished in the window (a folder's big file alone): by its bytes, nothing until 5 s") {
    speed::Pace p;
    double bytes = 0;
    for (int k = 0; k <= 19; ++k) { // up to 4.75 s
        p.sample(at(tick(k)), 0, static_cast<std::uint64_t>(bytes));
        bytes += 2 * MiB / 4;
    }
    CHECK_FALSE(p.model().has_value()); // not 5 s yet
    for (int k = 20; k <= 120; ++k) { // to 30 s
        p.sample(at(tick(k)), 0, static_cast<std::uint64_t>(bytes));
        bytes += 2 * MiB / 4;
    }
    REQUIRE(p.model().has_value());
    const auto m = p.model().value_or(speed::Pace::Model{.per_file = 1, .per_byte = 1});
    CHECK(m.per_file == 0);
    CHECK(m.per_byte == doctest::Approx(1 / (2 * MiB)).epsilon(0.01));
    CHECK(p.time_left(0, static_cast<std::uint64_t>(20 * MiB)).value_or(0) == doctest::Approx(10).epsilon(0.01));
}

TEST_CASE("time left: a single file, its bytes left at its speed once its bytes have come for 5 s") {
    const auto since = at(10);
    CHECK_FALSE(speed::file_left(std::nullopt, at(30), 1000, 100).has_value()); // no bytes yet
    CHECK_FALSE(speed::file_left(since, at(14.75), static_cast<std::uint64_t>(10 * MiB), MiB).has_value());
    CHECK(speed::file_left(since, at(15), static_cast<std::uint64_t>(10 * MiB), MiB).value_or(0) == doctest::Approx(10));
    CHECK_FALSE(speed::file_left(since, at(20), static_cast<std::uint64_t>(10 * MiB), 0).has_value());
    CHECK_FALSE(speed::file_left(since, at(20), 0, MiB).has_value());
}

TEST_CASE("time left: nothing came in the whole window, none") {
    speed::Pace p;
    for (int k = 0; k <= 120; ++k) p.sample(at(tick(k)), 7, static_cast<std::uint64_t>(5 * MiB));
    CHECK_FALSE(p.model().has_value());
    CHECK_FALSE(p.time_left(10, 1000).has_value());
}

namespace {

// A folder whose files each cost `per_file` seconds plus their bytes at `bandwidth` (bytes/s),
// fetched one after another as the pace adds up (the pipelined round trips of many in flight
// count as each file's share). Sizes from `size_of(k)`; `jitter` makes each tick's work vary by
// up to that share either way, as completions come in bursts.
struct Folder {
    double per_file = 0;
    double bandwidth = 0;
    std::vector<double> sizes;
    double jitter = 0;
    bool hold = false; // the jitter held for a second at a time (bursts that last), not drawn per tick
    double burst = 1;

    std::size_t next = 0;    // the file being fetched
    double into = 0;         // seconds of its work done
    std::uint64_t files = 0; // finished
    double bytes = 0;        // come
    double t = 0;

    [[nodiscard]] double work(std::size_t k) const { return per_file + sizes[k] / bandwidth; }
    // The true time left: the work of the files not finished.
    [[nodiscard]] double left() const {
        double w = -into;
        for (std::size_t k = next; k < sizes.size(); ++k) w += work(k);
        return w;
    }
    [[nodiscard]] std::uint64_t files_left() const { return sizes.size() - next; }
    [[nodiscard]] std::uint64_t bytes_left() const {
        double b = 0;
        for (std::size_t k = next; k < sizes.size(); ++k) b += sizes[k];
        if (next < sizes.size()) b -= into / work(next) * sizes[next]; // the part of it come
        return static_cast<std::uint64_t>(b);
    }
    // A tick of 250 ms (more or less work in it with jitter).
    void tick(Noise& noise) {
        t += 0.25;
        if (!hold || std::fmod(t, 1.0) < 0.125) burst = 1 + jitter * (2 * noise.next() - 1);
        double budget = 0.25 * burst;
        while (budget > 0 && next < sizes.size()) {
            const double need = work(next) - into;
            if (need <= budget) {
                budget -= need;
                bytes += sizes[next] * (need / work(next)); // its last bytes
                into = 0;
                ++next;
                ++files;
            } else {
                bytes += sizes[next] * (budget / work(next));
                into += budget;
                budget = 0;
            }
        }
    }
    [[nodiscard]] bool done() const { return next >= sizes.size(); }
};

} // namespace

TEST_CASE("time left: a folder of small files, paced by its files' round trips more than by its bytes") {
    // 5 ms a file plus 8 MiB/s; files of 2-200 KiB in runs of like ones (a folder of thumbnails, one
    // of photos, one of notes), so seconds of many tiny files and seconds of fewer bigger ones tell
    // the two apart.
    Noise noise;
    Folder f{.per_file = 0.005, .bandwidth = 8 * MiB, .sizes = {}, .jitter = 0.2};
    while (f.sizes.size() < 4000) {
        const double typical = 2 * std::pow(100.0, noise.next()); // KiB
        for (int k = 0; k < 60; ++k) f.sizes.push_back(typical * (0.8 + 0.4 * noise.next()) * KiB);
    }
    speed::Pace p;
    p.sample(at(0), 0, 0);
    while (f.t < 30) {
        f.tick(noise);
        p.sample(at(f.t), f.files, static_cast<std::uint64_t>(f.bytes));
    }
    const auto m = p.model().value_or(speed::Pace::Model{});
    MESSAGE("per file ", m.per_file * 1000, " ms (5), bandwidth ", 1 / m.per_byte / MiB, " MiB/s (8)");
    CHECK(std::abs(m.per_file / 0.005 - 1) < 0.25); // the bytes' floor takes some of it
    // The bandwidth: never above the truth (per_byte is at least the fastest second's).
    CHECK(1 / m.per_byte <= 8 * MiB * 1.02);
    CHECK(1 / m.per_byte > 8 * MiB * 0.6);
    // What's left: as many tiny files, then a few big ones. Bytes alone would put the tiny files at
    // almost nothing and the big ones at the small files' pace.
    const double truth = 3000 * (0.005 + KiB / (8 * MiB)) + 4 * (0.005 + 30 * MiB / (8 * MiB));
    const auto left = p.time_left(3004, static_cast<std::uint64_t>(3000 * KiB + 4 * 30 * MiB)).value_or(0);
    MESSAGE("time left ", left, " s, truly ", truth, " s");
    CHECK(left == doctest::Approx(truth).epsilon(0.15));
}

TEST_CASE("time left: files of one size can't tell files from bytes apart: the bytes take the time") {
    Noise noise;
    Folder f{.per_file = 0.01, .bandwidth = 4 * MiB, .sizes = {}, .jitter = 0.3};
    f.sizes.assign(5000, 16 * KiB);
    speed::Pace p;
    p.sample(at(0), 0, 0);
    while (f.t < 25) {
        f.tick(noise);
        p.sample(at(f.t), f.files, static_cast<std::uint64_t>(f.bytes));
    }
    const auto m = p.model().value_or(speed::Pace::Model{.per_file = 1, .per_byte = 1});
    MESSAGE("per file ", m.per_file * 1000, " ms, per MiB ", m.per_byte * MiB, " s");
    // Whichever way it splits, more files of that size come at the pace they came.
    const auto left = p.time_left(f.files_left(), f.bytes_left()).value_or(0);
    CHECK(left == doctest::Approx(f.left()).epsilon(0.1));
    CHECK(m.per_file < 0.003); // mostly the bytes'
}

TEST_CASE("time left: a folder's big files waiting behind its small ones never come out short") {
    // Only small files run (5 ms each, 0.1-20 KiB, 50 MB/s): seconds that can't tell files from
    // bytes apart, completions in bursts that last a second. Whatever split the fit finds, the
    // waiting big files' bytes cost at least what the fastest second's bytes did.
    for (std::uint64_t seed = 1; seed <= 20; ++seed) {
        Noise noise{.s = 0x9E3779B97F4A7C15ULL * seed};
        Folder f{.per_file = 0.005, .bandwidth = 50e6, .sizes = {}, .jitter = 0.3, .hold = true};
        for (int k = 0; k < 20000; ++k) f.sizes.push_back((0.1 + 19.9 * noise.next()) * KiB);
        speed::Pace p;
        p.sample(at(0), 0, 0);
        while (f.t < 30) {
            f.tick(noise);
            p.sample(at(f.t), f.files, static_cast<std::uint64_t>(f.bytes));
        }
        const double big = 100 * 1024 * MiB;
        const auto left = p.time_left(0, static_cast<std::uint64_t>(big)).value_or(0);
        CHECK_MESSAGE(left >= big / 50e6, "seed ", seed, ": ", left, " s");
    }
}

TEST_CASE("time left: a change in the link's speed shows within seconds, not after the 20 s window") {
    // A file-bound mix at 3 MiB/s, then the link six times as fast at 15 s (Main's swings). The
    // pace's costs scaled by the speed now (the Meter's ~5 s), through the countdown, as App does.
    Noise noise;
    Folder f{.per_file = 0.0005, .bandwidth = 3 * MiB, .sizes = {}, .jitter = 0.2};
    for (int k = 0; k < 6000; ++k) f.sizes.push_back((4 + 196 * noise.next()) * KiB);
    speed::Pace p;
    speed::Meter m;
    speed::Countdown c;
    p.sample(at(0), 0, 0);
    m.sample(at(0), 0);
    std::optional<double> shown;
    while (f.t < 27) {
        if (f.t >= 15) f.bandwidth = 18 * MiB;
        f.tick(noise);
        p.sample(at(f.t), f.files, static_cast<std::uint64_t>(f.bytes));
        m.sample(at(f.t), static_cast<std::uint64_t>(f.bytes));
        shown = c.next(at(f.t), p.time_left(f.files_left(), f.bytes_left(), m.rate()));
    }
    const double real = f.left();
    const double windowed = p.time_left(f.files_left(), f.bytes_left()).value_or(0);
    MESSAGE("12 s after: shown ", shown.value_or(0), " s, really ", real, " s (the window's average alone: ", windowed, " s)");
    CHECK(std::abs(shown.value_or(0) / real - 1) < 0.15);
    CHECK(windowed / real > 1.3); // what it showed before
}

TEST_CASE("time left: shown counting down, smoothed so a noisy estimate doesn't jump, blank without one") {
    speed::Countdown c;
    CHECK_FALSE(c.next(at(0), std::nullopt).has_value());
    // Right estimates: it simply counts down.
    for (int k = 0; k <= 40; ++k) CHECK(c.next(at(tick(k)), 100 - tick(k)).value_or(-1) == doctest::Approx(100 - tick(k)));
    // Noisy ones (±20 % around the truth): it moves little at a time and stays near the truth.
    Noise noise;
    double worst_step = 0;
    double worst_off = 0;
    double last = c.next(at(10.25), 89.75).value_or(-1);
    for (int k = 42; k <= 160; ++k) { // 10.5 s to 40 s
        const double t = tick(k);
        const double truth = 100 - t;
        const double shown = c.next(at(t), truth * (0.8 + 0.4 * noise.next())).value_or(-1);
        worst_step = std::max(worst_step, std::abs(shown - (last - 0.25)));
        worst_off = std::max(worst_off, std::abs(shown - truth) / truth);
        last = shown;
    }
    MESSAGE("largest step beyond the countdown ", worst_step, " s; furthest off ", worst_off * 100, " %");
    CHECK(worst_step < 2); // the estimates jump up to 18 s
    CHECK(worst_off < 0.1);
    // A real change (twice as fast): followed, most of the way within about 9 s.
    double shown = last;
    for (int k = 161; k <= 196; ++k) shown = c.next(at(tick(k)), (100 - 40) / 2.0 - (tick(k) - 40)).value_or(-1); // to 49 s
    CHECK(shown == doctest::Approx(30 - 9).epsilon(0.1));
    CHECK_FALSE(c.next(at(49.25), std::nullopt).has_value());
    CHECK(c.next(at(49.5), 12).value_or(-1) == 12); // after a blank, the next estimate as it is
    // The estimate stays (a tail slower than the speed): it doesn't count down to 0 below it.
    double tail = 12;
    for (int k = 1; k <= 80; ++k) tail = c.next(at(49.5 + tick(k)), 1.5).value_or(-1);
    CHECK(tail >= 1.5 / speed::countdown_band);
    CHECK(tail <= 1.5);
}

TEST_CASE("time left: a folder's run, the shown time against the real one at 25, 50 and 75 % done") {
    // A flush every 250 ms, as the window's: the pace learned from files and bytes, the time left
    // through the countdown. 2,000 files of 4-200 KiB, 4 ms each plus 6 MiB/s, with bursty ticks
    // (up to 30 % more or less work in each). The first estimates, from 5 s of it, are the least
    // sure.
    Noise noise;
    Folder f{.per_file = 0.004, .bandwidth = 6 * MiB, .sizes = {}, .jitter = 0.3};
    for (int k = 0; k < 2000; ++k) f.sizes.push_back((4 + 196 * noise.next()) * KiB);
    speed::Pace p;
    speed::Meter meter;
    speed::Countdown c;
    p.sample(at(0), 0, 0);
    meter.sample(at(0), 0);
    const double total = f.left();
    std::vector<double> errors;
    std::optional<double> first_shown;
    double mark = 0.25;
    while (!f.done()) {
        f.tick(noise);
        p.sample(at(f.t), f.files, static_cast<std::uint64_t>(f.bytes));
        meter.sample(at(f.t), static_cast<std::uint64_t>(f.bytes));
        const auto raw = p.time_left(f.files_left(), f.bytes_left(), meter.rate());
        const auto shown = c.next(at(f.t), raw);
        if (shown && !first_shown) first_shown = f.t;
        if (mark < 1 && f.t >= mark * total) {
            REQUIRE(shown.has_value());
            const double real = total - f.t;
            errors.push_back((shown.value_or(0) - real) / real);
            const double pct = mark * 100;
            MESSAGE(pct, " %: shown ", shown.value_or(0), " s, really ", real, " s");
            mark += 0.25;
        }
    }
    REQUIRE(first_shown.has_value());
    CHECK(first_shown.value_or(0) >= 5);    // blank until 5 s of progress
    CHECK(first_shown.value_or(0) <= 5.25);
    REQUIRE(errors.size() == 3);
    CHECK(std::abs(errors[0]) < 0.12);
    CHECK(std::abs(errors[1]) < 0.08);
    CHECK(std::abs(errors[2]) < 0.08);
}
