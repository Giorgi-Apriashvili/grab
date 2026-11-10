#include "progress_line.hpp"
#include "speed.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using namespace grab;

namespace {
constexpr std::uint64_t MiB = std::uint64_t{1} << 20;
}

TEST_CASE("durations read m:ss, or h:mm:ss from an hour") {
    CHECK(progress::duration(0) == "0:00");
    CHECK(progress::duration(26) == "0:26");
    CHECK(progress::duration(59.6) == "1:00");
    CHECK(progress::duration(723) == "12:03");
    CHECK(progress::duration(3723) == "1:02:03");
    CHECK(progress::duration(-5) == "0:00");
    CHECK(progress::duration(std::nan("")) == "0:00");
}

TEST_CASE("the live progress line") {
    progress::Status s;
    s.bytes = 412 * MiB;
    s.total = 898 * MiB;
    s.speed = 18.0 * MiB;
    s.eta = 26;
    s.connections = 14;
    CHECK(progress::line(s) == "412.0 MiB / 898.0 MiB  45%  18.0 MiB/s  14 conns  ETA 0:26");

    progress::Status start;
    start.total = 898 * MiB;
    start.connections = 1;
    CHECK(progress::line(start) == "0 B / 898.0 MiB  0%  1 conn"); // no speed or ETA yet

    s.files_done = 4;
    s.files = 13;
    CHECK(progress::line(s) == "4/13 files  412.0 MiB / 898.0 MiB  45%  18.0 MiB/s  14 conns  ETA 0:26");

    CHECK(progress::line(progress::Status{}) == "0 B / 0 B  0%  0 conns");
}

TEST_CASE("the line that ends a download") {
    CHECK(progress::done(898 * MiB, 898 * MiB, 48) == "done: 898.0 MiB in 0:48 (18.7 MiB/s)");
    // continued from an earlier run: the speed is this run's
    CHECK(progress::done(898 * MiB, 412 * MiB, 20) == "done: 898.0 MiB in 0:20 (412.0 MiB of it now, 20.6 MiB/s)");
    CHECK(progress::done(597 * MiB, 597 * MiB, 53, 13) == "done: 13 files, 597.0 MiB in 0:53 (11.3 MiB/s)");
    CHECK(progress::done(100, 100, 1, 1) == "done: 1 file, 100 B in 0:01 (100 B/s)");
    CHECK(progress::done(1024, 1024, 0.2) == "done: 1.0 KiB in 0:00"); // too quick for a speed
    CHECK(progress::done(0, 0, 1.0, 0) == "done: 0 files, 0 B in 0:01");
}

TEST_CASE("a line is fitted to the console without wrapping") {
    CHECK(progress::fit("abc", 3) == "abc");
    CHECK(progress::fit("abcdef", 4) == "abc\xE2\x80\xA6");
    CHECK(progress::fit("\xC3\xA4\xE2\x82\xAC" "x", 2) == "\xC3\xA4\xE2\x80\xA6"); // "ä€x" -> "ä…": columns, not bytes
    CHECK(progress::fit("\xC3\xA4\xE2\x82\xAC", 2) == "\xC3\xA4\xE2\x82\xAC");      // fits exactly
    CHECK(progress::fit("x", 0).empty());
}

namespace {

// A made-up run's clock: the line's 250 ms redraws from an arbitrary start.
const speed::Clock::time_point t0 = speed::Clock::time_point{} + std::chrono::hours(1000);
speed::Clock::time_point at(double seconds) {
    return t0 + std::chrono::duration_cast<speed::Clock::duration>(std::chrono::duration<double>(seconds));
}

// A folder's files fetched one after another, each `per_file` seconds (its round trips) plus its
// bytes at `bandwidth` (bytes/s), as many small files in flight add up.
struct Folder {
    std::vector<double> sizes;
    double per_file = 0;
    double bandwidth = 0;
    std::size_t next = 0; // the file being fetched
    double into = 0;      // seconds of its work done
    std::uint64_t files = 0;
    double bytes = 0;
    double t = 0;

    [[nodiscard]] double work(std::size_t k) const { return per_file + sizes[k] / bandwidth; }
    [[nodiscard]] double left() const {
        double w = -into;
        for (std::size_t k = next; k < sizes.size(); ++k) w += work(k);
        return w;
    }
    [[nodiscard]] bool done() const { return next >= sizes.size(); }
    // The counts the line is fed: its bytes come, files finished, and what is left of both.
    [[nodiscard]] progress::FolderMeter::Counts counts(std::uint64_t limit_changes = 0) const {
        double b = 0;
        for (std::size_t k = next; k < sizes.size(); ++k) b += sizes[k];
        if (!done()) b -= into / work(next) * sizes[next];
        return {.received = static_cast<std::uint64_t>(bytes),
                .finished = files,
                .left_files = sizes.size() - next,
                .left_bytes = static_cast<std::uint64_t>(std::max(0.0, b)),
                .limit_changes = limit_changes};
    }
    // 250 ms of fetching.
    void tick() {
        t += 0.25;
        double budget = 0.25;
        while (budget > 0 && !done()) {
            const double need = work(next) - into;
            const double spent = std::min(need, budget);
            bytes += sizes[next] * (spent / work(next));
            budget -= spent;
            into += spent;
            if (spent == need) {
                into = 0;
                ++next;
                ++files;
            }
        }
    }
};

// The same numbers every run.
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

TEST_CASE("a folder's line: a folder of small files has the speed of its bytes and a time left that holds") {
    // 3,000 files of 1-200 KiB (none has a speed of its own: the line's old sum of the big files'
    // speeds read 0 B/s and no time left), 4 ms each plus 6 MiB/s.
    Noise noise;
    Folder f{.sizes = {}, .per_file = 0.004, .bandwidth = 6.0 * MiB};
    for (int k = 0; k < 3000; ++k) f.sizes.push_back((1 + 199 * noise.next()) * 1024);
    progress::FolderMeter m;
    m.sample(at(0), f.counts());
    const double total = f.left();
    std::vector<double> history{0}; // the bytes come at each tick
    double mark = 0.25;
    while (!f.done()) {
        f.tick();
        m.sample(at(f.t), f.counts());
        history.push_back(f.bytes);
        if (f.t < 0.5) CHECK(m.speed() == 0); // less than half a second of bytes
        if (f.t >= 0.5) CHECK(m.speed() > 0);
        if (f.t < 5) CHECK_FALSE(m.eta().has_value()); // blank until 5 s of bytes
        if (f.t >= 5.25 && !f.done()) CHECK(m.eta().has_value());
        // The speed: what came over about the last 5 s.
        if (f.t >= 6 && !f.done()) {
            const double real = (history.back() - history[history.size() - 21]) / 5;
            CHECK(m.speed() == doctest::Approx(real).epsilon(0.05));
        }
        if (mark < 1 && f.t >= mark * total) {
            const double real = total - f.t;
            const double pct = mark * 100;
            const double mibps = m.speed() / MiB;
            MESSAGE(pct, " %: ETA ", m.eta().value_or(-1), " s, really ", real, " s; ", mibps, " MiB/s");
            CHECK(m.eta().value_or(0) == doctest::Approx(real).epsilon(0.15));
            const std::string text = progress::line({.bytes = f.counts().received,
                                                     .total = 0,
                                                     .speed = m.speed(),
                                                     .eta = m.eta(),
                                                     .connections = 4,
                                                     .files_done = f.files,
                                                     .files = f.sizes.size()});
            CHECK(text.find(" MiB/s  4 conns  ETA ") != std::string::npos);
            mark += 0.25;
        }
    }
    CHECK(m.eta().value_or(-1) == 0); // nothing left
}

TEST_CASE("a folder's line: the time spent connecting is neither its speed nor its pace") {
    Folder f{.sizes = std::vector<double>(500, 100.0 * 1024), .per_file = 0.002, .bandwidth = 4.0 * MiB};
    progress::FolderMeter m;
    for (int k = 0; k <= 12; ++k) { // 3 s logging in and listing: nothing comes
        m.sample(at(k * 0.25), f.counts());
        CHECK(m.speed() == 0);
        CHECK_FALSE(m.eta().has_value());
    }
    f.t = 3;
    while (f.t < 8.5) {
        f.tick();
        m.sample(at(f.t), f.counts());
        if (f.t >= 3.5) CHECK(m.speed() == doctest::Approx(f.bytes / (f.t - 3)).epsilon(0.02));
        // 5 s of its bytes, not 5 s from the start.
        if (f.t < 8.25) CHECK_FALSE(m.eta().has_value());
    }
    REQUIRE(m.eta().has_value());
    CHECK(m.eta().value_or(0) == doctest::Approx(f.left()).epsilon(0.1));
}

TEST_CASE("a folder's line: a new speed limit shows within about a second, its time left goes on") {
    // 8 MiB/s, then the speed schedule's 2 MiB/s from 10 s: the limiter's change count moves on.
    for (const bool counted : {true, false}) {
        CAPTURE(counted);
        Folder f{.sizes = std::vector<double>(2, 300.0 * MiB), .per_file = 0, .bandwidth = 8.0 * MiB};
        progress::FolderMeter m;
        m.sample(at(0), f.counts());
        while (f.t < 10) {
            f.tick();
            m.sample(at(f.t), f.counts());
        }
        REQUIRE(m.speed() == doctest::Approx(8 * MiB).epsilon(0.001));
        REQUIRE(m.eta().has_value());
        const std::uint64_t changes = counted ? 1 : 0;
        m.sample(at(f.t + 0.01), f.counts(changes)); // seen right after the change (no bytes yet)
        f.bandwidth = 2.0 * MiB;
        f.into = f.into * 4; // the same bytes of the file, in seconds at the new speed
        while (f.t < 11.5) {
            f.tick();
            m.sample(at(f.t), f.counts(changes));
            CHECK(m.speed() > 0); // the old speed, then the new one: never 0 in between
            CHECK(m.eta().has_value());
        }
        MESSAGE("1.5 s after the change: ", m.speed() / MiB, " MiB/s");
        if (counted) CHECK(m.speed() == doctest::Approx(2 * MiB).epsilon(0.02));
        else CHECK(m.speed() > 4 * MiB); // the 5 s average still fading
        while (f.t < 20) {
            f.tick();
            m.sample(at(f.t), f.counts(changes));
        }
        CHECK(m.speed() == doctest::Approx(2 * MiB).epsilon(0.02));
        // Bytes alone now (no file finished in the window), at the new speed.
        CHECK(m.eta().value_or(0) == doctest::Approx(f.left()).epsilon(0.15));
    }
}
