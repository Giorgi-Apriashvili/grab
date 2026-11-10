#include "speed.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
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

TEST_CASE("speed: restarted for a new limit, it shows the new speed within about a second, the old one until then") {
    speed::Meter m;
    speed::Meter plain; // the same bytes, never restarted
    double t = 0;
    double bytes = 0;
    auto run = [&](double rate, double seconds) {
        for (double end = t + seconds; t < end - 1e-9;) {
            t += 0.25;
            bytes += rate * 0.25;
            m.sample(at(t), static_cast<std::uint64_t>(bytes));
            plain.sample(at(t), static_cast<std::uint64_t>(bytes));
        }
    };
    m.sample(at(0), 0);
    plain.sample(at(0), 0);
    run(11 * MiB, 10);
    REQUIRE(m.rate() == doctest::Approx(11 * MiB).epsilon(0.001));
    const auto since = m.since();
    REQUIRE(since);
    // Switched on at 2 MiB/s: the last replies before the connections drop, nothing for 0.75 s
    // while they log in again, then 2 MiB/s.
    m.restart(at(t), static_cast<std::uint64_t>(bytes));
    run(1.2 * MiB, 0.25);
    CHECK(m.rate() == doctest::Approx(11 * MiB).epsilon(0.001)); // held: neither the new speed yet nor 0
    run(0, 0.75);
    CHECK(m.rate() == doctest::Approx(11 * MiB).epsilon(0.001));
    run(2 * MiB, 0.25);
    CHECK(m.rate() == doctest::Approx(11 * MiB).epsilon(0.001));
    run(2 * MiB, 0.25);
    // 1.5 s after the switch: the new speed, what came before the drop left out; the 5 s average
    // would still read about 7.
    CHECK(m.rate() == doctest::Approx(2 * MiB).epsilon(0.001));
    MESSAGE("never restarted: ", plain.rate() / MiB, " MiB/s");
    CHECK(plain.rate() > 5 * MiB);
    CHECK(m.since() == since); // the time left goes on
    run(2 * MiB, 5);
    CHECK(m.rate() == doctest::Approx(2 * MiB).epsilon(0.001));

    // Raised to 6 MiB/s with no gap: shown after half a second.
    m.restart(at(t), static_cast<std::uint64_t>(bytes));
    run(6 * MiB, 0.25);
    CHECK(m.rate() == doctest::Approx(2 * MiB).epsilon(0.001));
    run(6 * MiB, 0.25);
    CHECK(m.rate() == doctest::Approx(6 * MiB).epsilon(0.001));
    run(6 * MiB, 5);

    // Lowered to a trickle with gaps (a 32 KiB READ every half second): shown within a second too,
    // then its usual rules (a gap counts).
    m.restart(at(t), static_cast<std::uint64_t>(bytes));
    auto step = [&](double got) {
        t += 0.25;
        bytes += got;
        m.sample(at(t), static_cast<std::uint64_t>(bytes));
    };
    step(32 * KiB);
    CHECK(m.rate() == doctest::Approx(6 * MiB).epsilon(0.001));
    step(0);
    CHECK(m.rate() == doctest::Approx(6 * MiB).epsilon(0.001));
    step(32 * KiB);
    CHECK(m.rate() == doctest::Approx(64 * KiB / 0.75).epsilon(0.001));
    for (int k = 0; k < 12; ++k) {
        step(0);
        step(32 * KiB);
    }
    CHECK(m.rate() == doctest::Approx(64 * KiB).epsilon(0.05));
}

TEST_CASE("speed: restarted, with no bytes after it, it reads 0 again by meter_full") {
    for (const double first : {0.0, 0.25}) { // nothing at all; or a quarter second (the gap rule's way)
        CAPTURE(first);
        speed::Meter m;
        double t = 0;
        double bytes = 0;
        m.sample(at(0), 0);
        for (; t < 10 - 1e-9;) {
            t += 0.25;
            bytes += 11 * MiB * 0.25;
            m.sample(at(t), static_cast<std::uint64_t>(bytes));
        }
        REQUIRE(m.rate() == doctest::Approx(11 * MiB).epsilon(0.001));
        const double restarted = t;
        m.restart(at(t), static_cast<std::uint64_t>(bytes));
        for (; t < restarted + first - 1e-9;) {
            t += 0.25;
            bytes += 2 * MiB * 0.25;
            m.sample(at(t), static_cast<std::uint64_t>(bytes));
        }
        // Logging in again fails: the download waits out its backoff, still running.
        for (; t < restarted + 3.75 - 1e-9;) {
            t += 0.25;
            m.sample(at(t), static_cast<std::uint64_t>(bytes));
            CHECK(m.rate() == doctest::Approx(11 * MiB).epsilon(0.001)); // held, not 0 in between
        }
        for (; t < restarted + first + 5 - 1e-9;) {
            t += 0.25;
            m.sample(at(t), static_cast<std::uint64_t>(bytes));
        }
        CHECK(m.rate() == 0);
        CHECK(!m.since());
    }
}

TEST_CASE("speed: restarted, a first burst leaves the shown speed within about a second") {
    speed::Meter m;
    double t = 0;
    double bytes = 0;
    auto run = [&](double rate, double seconds) {
        for (double end = t + seconds; t < end - 1e-9;) {
            t += 0.25;
            bytes += rate * 0.25;
            m.sample(at(t), static_cast<std::uint64_t>(bytes));
        }
    };
    m.sample(at(0), 0);
    run(6 * MiB, 10);
    // Lowered to 1 MiB/s: 1.5 MiB on its way comes in the first quarter second, then the debt
    // it left holds the bytes back for a while, then 1 MiB/s.
    m.restart(at(t), static_cast<std::uint64_t>(bytes));
    const double changed = t;
    run(6 * MiB, 0.25);
    run(0.25 * MiB, 0.75);
    run(1 * MiB, 1);
    CHECK(t == doctest::Approx(changed + 2));
    CHECK(m.rate() == doctest::Approx(1 * MiB).epsilon(0.001)); // the drain and the debt are out of its window
    run(1 * MiB, 3);
    CHECK(m.rate() == doctest::Approx(1 * MiB).epsilon(0.001));
    // A connection gone quiet in its first seconds reads 0, its time left kept.
    m.restart(at(t), static_cast<std::uint64_t>(bytes));
    run(1 * MiB, 0.5);
    REQUIRE(m.rate() == doctest::Approx(1 * MiB).epsilon(0.001));
    run(0, 1.25);
    CHECK(m.rate() == 0);
    CHECK(m.since());
    run(1 * MiB, 0.5);
    CHECK(m.rate() == doctest::Approx(0.5 * MiB).epsilon(0.001)); // half of its last second
    run(1 * MiB, 5);
    CHECK(m.rate() == doctest::Approx(1 * MiB).epsilon(0.001));
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
    // A new speed (a limit set, raised or off): the file being fetched keeps the share of it done.
    void set_bandwidth(double b) {
        const double share = done() ? 0 : into / work(next);
        bandwidth = b;
        if (!done()) into = share * work(next);
    }
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

namespace {

// A row at each flush as App::meter has it: its speed, and its time left from its pace (a folder)
// or from its bytes (a file), through the countdown, held while a new limit's speed doesn't show
// yet, or from the limit at once when it is below the speed then; restart() as App::set_rate does
// at a new limit (`limit` in bytes/s, 0: none, or not one it may foresee). `fixed` false: as before
// the pace started again at a new limit, for the comparison.
struct Row {
    bool folder = true;
    bool fixed = true;
    speed::Meter meter;
    speed::Pace pace;
    speed::Countdown countdown;
    std::optional<double> eta;

    void restart(Clock::time_point now, std::uint64_t bytes, double limit) {
        meter.restart(now, bytes, fixed ? limit : 0);
        if (!fixed) return;
        pace.restart();
        countdown.restart();
    }
    void sample(Clock::time_point now, std::uint64_t files, std::uint64_t bytes, std::uint64_t files_left,
                std::uint64_t bytes_left) {
        meter.sample(now, bytes);
        const bool holding = fixed && meter.holding();
        if (holding && !meter.foreseen()) {
            eta = countdown.hold(now);
            return;
        }
        const double rate = meter.ahead();
        std::optional<double> left;
        if (folder) {
            if (meter.since() && !holding) pace.sample(now, files, bytes);
            left = pace.time_left(files_left, bytes_left, rate);
        } else {
            left = speed::file_left(meter.since(), now, bytes_left, rate);
        }
        eta = countdown.next(now, left);
    }
    void sample(Clock::time_point now, const Folder& f) {
        sample(now, f.files, static_cast<std::uint64_t>(f.bytes), f.files_left(), f.bytes_left());
    }
};

// How the time shown followed a new limit.
struct Followed {
    bool blank = false;    // it went blank after the change
    double worst_off = 0;  // the furthest off the real time left, as a share of it, from 3 s after
    double worst_fall = 0; // the most it fell in a tick beyond the tick's own 250 ms, as a share of the real time left
    double at3 = 0;        // shown / real, 3 s after
};

// `f` at `before` bytes/s for 15 s, then at `after` from a new limit (`limit`: Row::restart's), for
// 30 s more or until done. The fall counts from the change when slowed down (the time left jumps
// up), from 3 s after it when sped up (it jumps down, rightly).
Followed follow(Folder f, bool folder, bool fixed, double before, double after, double limit) {
    Noise noise;
    Row row;
    row.folder = folder;
    row.fixed = fixed;
    f.bandwidth = before;
    row.sample(at(0), f);
    while (f.t < 15 - 1e-9) {
        f.tick(noise);
        row.sample(at(f.t), f);
    }
    REQUIRE(row.eta.has_value());
    f.set_bandwidth(after);
    row.restart(at(f.t), static_cast<std::uint64_t>(f.bytes), limit);
    Followed out;
    double last = row.eta.value_or(0);
    while (f.t < 45 - 1e-9 && !f.done()) {
        f.tick(noise);
        row.sample(at(f.t), f);
        if (!row.eta) {
            out.blank = true;
            continue;
        }
        const double shown = *row.eta;
        const double real = f.left();
        if (real < 5) break; // its last seconds: a share of them is no measure
        const double since = f.t - 15;
        if (since >= 3) out.worst_off = std::max(out.worst_off, std::abs(shown / real - 1));
        if (since >= 3 && since < 3.25) out.at3 = shown / real;
        if (since >= 3 || after < before) out.worst_fall = std::max(out.worst_fall, (last - shown - 0.25) / real);
        last = shown;
    }
    return out;
}

// The downloads the limit changes under: a single big file; a folder's two big files; a folder of
// small files and then two big ones (the live check's: its small files finished in the window
// before the change, at any of the speeds, and the old pace mixed with the new one went wrong); a
// folder of small files only.
struct Case {
    std::string name;
    bool folder;
    Folder f;
};

std::vector<Case> limit_cases() {
    Noise noise;
    std::vector<Case> cases;
    cases.push_back({.name = "a big file", .folder = false,
                     .f = Folder{.per_file = 0, .bandwidth = 0, .sizes = {1500 * MiB}, .jitter = 0.1}});
    cases.push_back({.name = "a folder's two big files", .folder = true,
                     .f = Folder{.per_file = 0.004, .bandwidth = 0, .sizes = {600 * MiB, 600 * MiB}, .jitter = 0.1}});
    Folder mixed{.per_file = 0.004, .bandwidth = 0, .sizes = {}, .jitter = 0.1};
    for (int k = 0; k < 150; ++k) mixed.sizes.push_back((1 + 199 * noise.next()) * KiB);
    mixed.sizes.push_back(300 * MiB);
    mixed.sizes.push_back(300 * MiB);
    cases.push_back({.name = "a folder of small files, then big ones", .folder = true, .f = mixed});
    Folder small_files{.per_file = 0.004, .bandwidth = 0, .sizes = {}, .jitter = 0.1};
    for (int k = 0; k < 3000; ++k) small_files.sizes.push_back((1 + 199 * noise.next()) * KiB);
    cases.push_back({.name = "a folder of small files", .folder = true, .f = small_files});
    return cases;
}

} // namespace

TEST_CASE("time left: a new limit, set, raised or off, shows in the time left within seconds, with no dip") {
    // 15 s at one speed, then a new limit (the live check's: 15 MiB/s, then 2 MiB/s): from 3 s
    // after the change, the time shown is within 15 % of the real one, and it never falls faster
    // than the time passing, beyond a little smoothing (it fell from 275 s to 92 s in 13 s).
    // The set also as when downloads side by side share the limit (no limit to foresee the speed
    // by: the time left waits for the new speed to show).
    struct Change {
        std::string name;
        double before;
        double after;
        double limit;
    };
    for (const auto& change : {Change{.name = "set", .before = 15 * MiB, .after = 2 * MiB, .limit = 2 * MiB},
                               Change{.name = "set, side by side", .before = 15 * MiB, .after = 2 * MiB, .limit = 0},
                               Change{.name = "raised", .before = 2 * MiB, .after = 15 * MiB, .limit = 15 * MiB},
                               Change{.name = "off", .before = 2 * MiB, .after = 20 * MiB, .limit = 0}}) {
        for (const auto& c : limit_cases()) {
            CAPTURE(change.name);
            CAPTURE(c.name);
            const auto got = follow(c.f, c.folder, true, change.before, change.after, change.limit);
            MESSAGE(change.name, ", ", c.name, ": 3 s after ", got.at3, " of the real time left; at worst ", got.worst_off * 100,
                    " % off, fell ", got.worst_fall * 100, " % of it in a tick beyond the time passing");
            CHECK_FALSE(got.blank);
            CHECK(got.worst_off < 0.15);
            CHECK(got.worst_fall < 0.01);
        }
    }
}

TEST_CASE("time left: before the pace started again at a new limit, it fell far too fast") {
    // The same change as the live check's, the old way: the window mixing both speeds.
    const auto cases = limit_cases();
    const auto& mixed = cases[2];
    const auto old = follow(mixed.f, mixed.folder, false, 15 * MiB, 2 * MiB, 2 * MiB);
    const auto now = follow(mixed.f, mixed.folder, true, 15 * MiB, 2 * MiB, 2 * MiB);
    MESSAGE("the old way at worst ", old.worst_off * 100, " % off; now ", now.worst_off * 100, " %");
    CHECK(old.worst_off > 0.2);
    CHECK(now.worst_off < 0.15);
}

namespace {

// A download as the live check's at a lowered limit: 15 s at 12.6 MiB/s, then a limit of 2 MiB/s.
// The replies on their way (6 MiB) come in the next 250 ms, the limiter then pauses for them until
// 2 s after the change, and its first second comes at 2.7 MiB/s (its bucket's burst): the first
// speed measured after the change is a third too high. The bytes come at each tick, from 0.
struct Lowered {
    double size = 0;
    std::vector<double> bytes;
    double end = 0; // when the last byte came
};

Lowered lowered(double size) {
    Noise noise;
    Lowered out{.size = size, .bytes = {0}, .end = 0};
    double got = 0;
    double t = 0;
    while (got < size) {
        t += 0.25;
        double rate = 12.6 * MiB;
        if (t > 15) rate = t <= 15.25 ? 24 * MiB : t <= 17 ? 0 : t <= 18 ? 2.7 * MiB : 2 * MiB;
        got = std::min(size, got + 0.25 * rate * (1 + 0.05 * (2 * noise.next() - 1)));
        out.bytes.push_back(got);
    }
    out.end = t;
    return out;
}

// How its time left followed the change.
struct AfterLowered {
    double worst_off = 0;  // the furthest off the real time left, as a share of it, from `from` s after
    double worst_fall = 0; // the most it fell in a tick beyond the tick's own 250 ms, as a share of it
};

AfterLowered after_lowered(const Lowered& d, bool folder, double limit, double from) {
    Row row;
    row.folder = folder;
    AfterLowered out;
    std::optional<double> last;
    for (std::size_t k = 0; k < d.bytes.size(); ++k) {
        const double t = 0.25 * static_cast<double>(k);
        const auto bytes = static_cast<std::uint64_t>(d.bytes[k]);
        if (k == 61) row.restart(at(15), static_cast<std::uint64_t>(d.bytes[60]), limit);
        row.sample(at(t), 0, bytes, 1, static_cast<std::uint64_t>(d.size) - bytes);
        if (t <= 15 || !row.eta) continue;
        const double real = d.end - t;
        if (real < 5) break;
        if (t - 15 >= from) out.worst_off = std::max(out.worst_off, std::abs(*row.eta / real - 1));
        if (last) out.worst_fall = std::max(out.worst_fall, (*last - *row.eta - 0.25) / real);
        last = row.eta;
    }
    return out;
}

} // namespace

TEST_CASE("time left: a limit below the speed shows at once, though the first speed measured after it is off by a third") {
    // The live check's: from the first speed measured, the time left was 22-25 % short 2-3 s after
    // the change, and inside 15 % only from about 4 s. A download fetching alone knows its new
    // speed at once: the limit.
    const auto d = lowered(1000 * MiB);
    for (const bool folder : {false, true}) {
        CAPTURE(folder);
        const auto foreseen = after_lowered(d, folder, 2 * MiB, 0.25);
        const std::string what = folder ? "a folder" : "a file";
        MESSAGE(what, ": from the limit, at worst ", foreseen.worst_off * 100,
                " % off from the change, fell ", foreseen.worst_fall * 100, " %");
        CHECK(foreseen.worst_off < 0.05);
        CHECK(foreseen.worst_fall < 0.01);
        const auto measured = after_lowered(d, folder, 0, 3);
        MESSAGE("from the speed measured: ", measured.worst_off * 100, " % off from 3 s");
        CHECK(measured.worst_off > 0.15); // the gap the limit closes
        CHECK(measured.worst_fall < 0.01);
    }
}
