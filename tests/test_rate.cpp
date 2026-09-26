#include "rate.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stop_token>
#include <thread>

using namespace grab;
using namespace std::chrono_literals;
using Clock = rate::TokenBucket::Clock;

constexpr std::uint64_t KiB = 1024;
constexpr std::uint64_t MiB = 1024 * 1024;

TEST_CASE("rates as grab --limit and rclone write them") {
    CHECK(rate::parse_rate("5M") == 5 * MiB);
    CHECK(rate::parse_rate("5m") == 5 * MiB);
    CHECK(rate::parse_rate("800K") == 800 * KiB);
    CHECK(rate::parse_rate("2.5M") == 5 * MiB / 2);
    CHECK(rate::parse_rate("1G") == 1024 * MiB);
    CHECK(rate::parse_rate("3") == 3 * MiB); // bare: MiB/s
    CHECK(rate::parse_rate(" 0.5 ") == MiB / 2);
    CHECK_FALSE(rate::parse_rate("").has_value());
    CHECK_FALSE(rate::parse_rate("0").has_value());
    CHECK_FALSE(rate::parse_rate("-1M").has_value());
    CHECK_FALSE(rate::parse_rate("fast").has_value());
    CHECK_FALSE(rate::parse_rate("5MB").has_value());
    CHECK(rate::bwlimit_arg(5 * MiB) == "5120K");
    CHECK(rate::bwlimit_arg(1500) == "2K"); // whole KiB, rounded up
    CHECK(rate::bwlimit_arg(1) == "1K");
}

TEST_CASE("the bucket holds its rate over time") {
    rate::TokenBucket b;
    auto t = Clock::time_point{} + 100s;
    b.set_rate(MiB, t);
    CHECK(b.burst() == doctest::Approx(static_cast<double>(MiB) / 4.0));
    // Take 64 KiB pieces as fast as allowed for 10 MiB: that takes ~10 s at 1 MiB/s.
    const auto start = t;
    std::uint64_t moved = 0;
    while (moved < 10 * MiB) {
        const auto wait = b.take(64 * KiB, t);
        if (wait == Clock::duration::zero()) moved += 64 * KiB;
        else t += wait;
    }
    CHECK(std::chrono::duration<double>(t - start).count() == doctest::Approx(10.0).epsilon(0.02));
}

TEST_CASE("an idle bucket allows only a short burst") {
    rate::TokenBucket b;
    auto t = Clock::time_point{} + 100s;
    b.set_rate(MiB, t);
    t += 60s; // a minute idle banks at most a quarter second
    CHECK(b.take(MiB / 4, t) == Clock::duration::zero());
    CHECK(b.take(1, t) > Clock::duration::zero());
}

TEST_CASE("bytes moved elsewhere slow the rest down, and unlimited never waits") {
    rate::TokenBucket b;
    auto t = Clock::time_point{} + 100s;
    b.set_rate(MiB, t);
    t += 1s;
    b.debit(2 * MiB, t); // an rclone batch moved 2 MiB meanwhile
    const auto wait = b.take(64 * KiB, t);
    CHECK(std::chrono::duration<double>(wait).count() == doctest::Approx(2.0 - 0.25 + 0.0625).epsilon(0.01));
    b.set_rate(0, t);
    CHECK(b.take(100 * MiB, t) == Clock::duration::zero());
}

TEST_CASE("fair queuing serves busy flows in proportion to their weights") {
    rate::FairQueue q;
    // Flow 1 (high, 4) and flow 2 (low, 1) always have one 64 KiB request waiting.
    auto a = q.add(1, 64 * KiB, 4);
    auto b = q.add(2, 64 * KiB, 1);
    int served[3] = {};
    for (int i = 0; i < 500; ++i) {
        const auto head = q.head();
        q.serve(head);
        ++served[head.flow];
        if (head.flow == 1) a = q.add(1, 64 * KiB, 4);
        else b = q.add(2, 64 * KiB, 1);
    }
    CHECK(static_cast<double>(served[1]) / served[2] == doctest::Approx(4.0).epsilon(0.05));

    // Equal weights take turns.
    rate::FairQueue even;
    even.add(1, 100, 2);
    even.add(2, 100, 2);
    int last = 0;
    int switches = 0;
    for (int i = 0; i < 20; ++i) {
        const auto head = even.head();
        even.serve(head);
        if (head.flow != last) ++switches;
        last = head.flow;
        even.add(head.flow, 100, 2);
    }
    CHECK(switches >= 19);
}

TEST_CASE("a flow that was idle gains no credit to burst with") {
    rate::FairQueue q;
    for (int i = 0; i < 50; ++i) q.serve(q.add(1, 64 * KiB, 2)); // flow 1 alone for a while
    // Flow 2 arrives: it starts at the current virtual time, not at zero, so it alternates
    // with flow 1 instead of taking the next 50 turns.
    q.add(1, 64 * KiB, 2);
    q.add(2, 64 * KiB, 2);
    int flow2_first = 0;
    for (int i = 0; i < 6; ++i) {
        const auto head = q.head();
        q.serve(head);
        flow2_first += head.flow == 2 ? 1 : 0;
        q.add(head.flow, 64 * KiB, 2);
    }
    CHECK(flow2_first == 3);
}

TEST_CASE("under a limit, a high download gets about four times a low one") {
    rate::RateLimiter limiter;
    limiter.set_rate(4 * MiB);
    std::stop_source stop;
    std::atomic<std::uint64_t> bytes[3] = {};
    auto pump = [&](int flow, int weight) {
        while (limiter.acquire(64 * KiB, stop.get_token(), flow, weight)) bytes[flow] += 64 * KiB;
    };
    {
        std::jthread high([&] { pump(1, 4); });
        std::jthread low([&] { pump(2, 1); });
        std::this_thread::sleep_for(1500ms);
        stop.request_stop();
    }
    const double ratio = static_cast<double>(bytes[1].load()) / static_cast<double>(bytes[2].load());
    CHECK(ratio > 3.0);
    CHECK(ratio < 5.5);
    const double total_mib = static_cast<double>(bytes[1] + bytes[2]) / MiB;
    CHECK(total_mib < 4 * 1.5 + 1.0); // the limit holds (plus the initial burst)
}

TEST_CASE("waiting streams go on when the limit is lifted, and stop when asked") {
    rate::RateLimiter limiter;
    limiter.set_rate(64 * KiB); // one piece per second
    std::stop_source never;
    REQUIRE(limiter.acquire(0, never.get_token()));
    std::atomic<bool> done{false};
    std::jthread waiter([&] {
        limiter.acquire(10 * MiB, never.get_token()); // ~160 s at this rate
        done = true;
    });
    std::this_thread::sleep_for(100ms);
    CHECK_FALSE(done.load());
    limiter.set_rate(0); // switched off: through at once
    waiter.join();
    CHECK(done.load());

    limiter.set_rate(64 * KiB);
    std::stop_source stop;
    std::atomic<bool> result{true};
    std::jthread stopped([&] { result = limiter.acquire(10 * MiB, stop.get_token()); });
    std::this_thread::sleep_for(100ms);
    stop.request_stop();
    stopped.join();
    CHECK_FALSE(result.load());
}
