#include "rate.hpp"

#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

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
    CHECK(b.take(0, t) == Clock::duration::zero()); // the first request: it fills from here
    t += 60s; // a minute idle banks at most a quarter second
    CHECK(b.take(MiB / 4, t) == Clock::duration::zero());
    CHECK(b.take(1, t) > Clock::duration::zero());
}

TEST_CASE("bytes moved elsewhere slow the rest down, and unlimited never waits") {
    rate::TokenBucket b;
    auto t = Clock::time_point{} + 100s;
    b.set_rate(MiB, t);
    CHECK(b.take(0, t) == Clock::duration::zero());
    t += 1s;
    b.debit(2 * MiB, t); // a stream's READs on their way as the limit came: 2 MiB
    const auto wait = b.take(64 * KiB, t);
    CHECK(std::chrono::duration<double>(wait).count() == doctest::Approx(2.0 - 0.25 + 0.0625).epsilon(0.01));
    b.set_rate(0, t);
    CHECK(b.take(100 * MiB, t) == Clock::duration::zero());
}

TEST_CASE("a request asked for before the rate was lowered still passes, and is paid back") {
    rate::TokenBucket b;
    auto t = Clock::time_point{} + 100s;
    b.set_rate(6 * MiB, t);
    CHECK(b.take(0, t) == Clock::duration::zero());
    t += 1s;
    CHECK(b.take(MiB, t) == Clock::duration::zero());
    b.set_rate(3 * MiB / 2, t); // the burst is 384 KiB now, less than one 1 MiB pipe read
    const auto start = t;
    std::uint64_t moved = 0;
    while (moved < 10 * MiB) { // in 1 MiB pieces: each waits for a full bucket, then pays back
        const auto wait = b.take(MiB, t);
        REQUIRE(wait < 10s);
        if (wait == Clock::duration::zero()) moved += MiB;
        else t += wait;
    }
    // The lowered limit starts empty: the first piece waits for a full bucket (a quarter second),
    // the rest on debt; every further MiB waits until it is paid for at 1.5 MiB/s.
    CHECK(std::chrono::duration<double>(t - start).count() == doctest::Approx(0.25 + 9.0 / 1.5).epsilon(0.01));
}

TEST_CASE("a limit switched on or lowered starts empty and fills only from the next request") {
    rate::TokenBucket b;
    auto t = Clock::time_point{} + 100s;
    // Switched on: what came unlimited leaves no credit, and none builds up before streams ask
    // (dropped ones log in again meanwhile), so the first piece waits its own time.
    b.set_rate(2 * MiB, t);
    t += 3s;
    auto wait = b.take(256 * KiB, t);
    CHECK(std::chrono::duration<double>(wait).count() == doctest::Approx(0.125).epsilon(0.01));
    t += wait;
    CHECK(b.take(256 * KiB, t) == Clock::duration::zero());
    // Running at the limit, then idle long enough to fill (a quarter second's worth)...
    t += 2s;
    // ...lowered: empty again, and still not filling until asked.
    b.set_rate(MiB, t);
    t += 1s;
    wait = b.take(128 * KiB, t);
    CHECK(std::chrono::duration<double>(wait).count() == doctest::Approx(0.125).epsilon(0.01));
    // A debt is kept through a lowering (bytes still owed), and forgiven by an unlimited spell.
    b.debit(MiB, t);
    b.set_rate(MiB / 2, t);
    wait = b.take(64 * KiB, t);
    CHECK(std::chrono::duration<double>(wait).count() == doctest::Approx(2.125).epsilon(0.01));
    b.set_rate(0, t);
    b.set_rate(MiB, t);
    wait = b.take(128 * KiB, t);
    CHECK(std::chrono::duration<double>(wait).count() == doctest::Approx(0.125).epsilon(0.01));
    // Raised: what is in the bucket stays, and it goes on filling.
    t += wait;
    CHECK(b.take(128 * KiB, t) == Clock::duration::zero());
    t += 1s; // full: a quarter second of 1 MiB/s
    b.set_rate(2 * MiB, t);
    CHECK(b.take(256 * KiB, t) == Clock::duration::zero());
    t += 250ms;
    CHECK(b.take(512 * KiB, t) == Clock::duration::zero());
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
    std::uint64_t last = 0;
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

TEST_CASE("a folder's files share its turns by their parts: its share stays") {
    // A normal folder (2) with a High and a Low file running (parts 4 and 1) against a normal
    // download (2), all busy.
    const auto high = rate::flow_of(7, 1);
    const auto low = rate::flow_of(7, 2);
    const auto other = rate::flow_of(8);
    CHECK(rate::flow_of(7) != rate::flow_of(7, 1));
    CHECK(rate::flow_of(-5, 3) != rate::flow_of(5, 3)); // a listing's negative id is its own
    CHECK(rate::flow_of(1, 0) == (std::uint64_t{1} << 32));
    rate::FairQueue q;
    const std::map<std::uint64_t, double> part{{high, 4}, {low, 1}, {other, 0}};
    for (const auto& [flow, p] : part) q.add(flow, 64 * KiB, 2, p);
    std::map<std::uint64_t, int> served;
    for (int i = 0; i < 1000; ++i) {
        const auto head = q.head();
        q.serve(head);
        ++served[head.flow];
        q.add(head.flow, 64 * KiB, 2, part.at(head.flow));
    }
    CHECK(static_cast<double>(served[high] + served[low]) / served[other] == doctest::Approx(1.0).epsilon(0.05));
    CHECK(static_cast<double>(served[high]) / served[low] == doctest::Approx(4.0).epsilon(0.1));
    // A folder's file alone takes all of the folder's turns, as many files do: no more.
    rate::FairQueue alone;
    alone.add(high, 64 * KiB, 2, 4);
    alone.add(other, 64 * KiB, 2);
    int folder_turns = 0;
    for (int i = 0; i < 1000; ++i) {
        const auto head = alone.head();
        alone.serve(head);
        folder_turns += head.flow == high ? 1 : 0;
        alone.add(head.flow, 64 * KiB, 2, head.flow == high ? 4 : 0);
    }
    CHECK(folder_turns == doctest::Approx(500).epsilon(0.02));
    // Weights below 1 are kept as they are; none or a negative one counts as 1.
    rate::FairQueue tiny;
    CHECK(tiny.add(1, 100, 0.25).start == 0);
    CHECK(tiny.add(1, 100, 0.25).start == doctest::Approx(400));
    CHECK(tiny.add(2, 100, -3).start == 0);
    CHECK(tiny.add(2, 100, 0).start == doctest::Approx(100));
}

TEST_CASE("a folder's files starting one after another don't give it more than its share") {
    // As in a folder beside a download under a limit that starts empty: the download's four streams
    // put a request each in line at once, then the folder's files come one by one as they log in (a
    // High one, a Normal one, two small workers), and every stream puts its next request in line as
    // its last is served. With each file a flow of its own at its part of the folder's weight among
    // those reading when it asked, the first file's requests kept the whole folder's weight beside
    // the later ones' and the folder took 0.56 of the first 4 MiB (0.6 of a share test's first half
    // second under load).
    const auto single = rate::flow_of(8);
    const std::vector<std::pair<std::uint64_t, double>> files{{rate::flow_of(7, 1), 4},
                                                              {rate::flow_of(7, 2), 2},
                                                              {rate::flow_of(7, 0x80000000), 2},
                                                              {rate::flow_of(7, 0x80000001), 2}};
    rate::FairQueue q;
    for (int i = 0; i < 4; ++i) q.add(single, 64 * KiB, 2);
    std::map<std::uint64_t, double> part{{single, 0}};
    std::uint64_t folder = 0;
    std::uint64_t all = 0;
    double early = 0;
    for (std::size_t i = 0; all < 32 * MiB; ++i) {
        if (i % 2 == 0 && i / 2 < files.size()) { // one more file every other turn
            const auto [flow, p] = files[i / 2];
            part[flow] = p;
            q.add(flow, 128 * KiB, 2, p);
        }
        const auto head = q.head();
        q.serve(head);
        all += head.n;
        if (head.flow != single) folder += head.n;
        q.add(head.flow, head.n, 2, part.at(head.flow));
        if (early == 0 && all >= 4 * MiB) early = static_cast<double>(folder) / static_cast<double>(all);
    }
    const double late = static_cast<double>(folder) / static_cast<double>(all);
    MESSAGE("the folder's share of the first 4 MiB ", early, ", of 32 MiB ", late);
    // Plain bounds: Approx's epsilon is relative to 1 + the value, so 0.04 would let 0.56 through.
    CHECK(early > 0.48);
    CHECK(early < 0.52);
    CHECK(late > 0.495);
    CHECK(late < 0.505);
}

TEST_CASE("a request given up costs its flow nothing") {
    rate::FairQueue q;
    const auto a = q.add(1, 100, 1);
    const auto b = q.add(1, 100, 1); // starts where a finishes
    CHECK(b.start == doctest::Approx(100));
    q.cancel(b); // the flow's last: its next starts where b would have
    CHECK(q.add(1, 100, 1).start == doctest::Approx(100));
    q.cancel(a); // not the last: the flow's place stays
    CHECK(q.add(1, 100, 1).start == doctest::Approx(200));
    q.cancel(a); // twice: nothing
    CHECK(q.add(1, 100, 1).start == doctest::Approx(300));
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
    // Each flow keeps requests in line, as a stream keeps its next READ's place while it reads
    // replies (enqueue, then wait), and whoever waits serves the head of the line: a flow whose
    // thread waits for the CPU keeps its turns. One request at a time (acquire), a flow was out of
    // line from each turn until its thread ran again, on a busy machine after whole time slices of
    // other threads, and the other flow took its turns (High : Low from 0.2 to 1.5); so it was
    // while a thread started, both put in line before either runs.
    constexpr std::size_t in_line = 16; // a third of a second of High's turns
    std::stop_source stop;
    std::array<std::uint64_t, 3> bytes{};
    std::array<std::deque<std::uint64_t>, 3> lines; // each flow's tickets, oldest first: served in that order
    const auto start = Clock::now();
    for (std::size_t i = 0; i < in_line; ++i) {
        lines[1].push_back(limiter.enqueue(64 * KiB, 1, 4));
        lines[2].push_back(limiter.enqueue(64 * KiB, 2, 1));
    }
    auto pump = [&](int flow, int weight) {
        auto& line = lines[flow];
        // Until its next ticket is given up (the end, below) or the stop: those served before.
        while (limiter.wait(line.front(), stop.get_token())) {
            bytes[flow] += 64 * KiB;
            line.pop_front();
            line.push_back(limiter.enqueue(64 * KiB, flow, weight));
        }
        for (const auto ticket : line) limiter.cancel(ticket);
    };
    double seconds = 0;
    {
        std::jthread high([&] { pump(1, 4); });
        std::jthread low([&] { pump(2, 1); });
        std::this_thread::sleep_for(1500ms);
        // The end: a lower rate gives up every request not served yet and serves none for now, so
        // what each flow counts is what was served by then, however late its thread runs.
        limiter.set_rate(1);
        seconds = std::chrono::duration<double>(Clock::now() - start).count();
        stop.request_stop();
    }
    const double ratio = static_cast<double>(bytes[1]) / static_cast<double>(bytes[2]);
    MESSAGE("High : Low = ", ratio, " over ", seconds, " s");
    CHECK(ratio > 3.0);
    CHECK(ratio < 5.5);
    const double total_mib = static_cast<double>(bytes[1] + bytes[2]) / MiB;
    CHECK(total_mib < 4 * seconds + 1.0); // the limit holds (plus the initial burst)
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

TEST_CASE("the same rate again changes nothing; a new one is seen by its generation") {
    rate::RateLimiter limiter;
    const auto first = limiter.setting();
    CHECK(first.rate == 0);
    limiter.set_rate(0); // the speed schedule sets the rate every second
    CHECK(limiter.generation() == first.generation);
    limiter.set_rate(2 * MiB);
    const auto on = limiter.setting();
    CHECK(on.rate == 2 * MiB);
    CHECK(on.generation != first.generation);
    limiter.set_rate(2 * MiB);
    CHECK(limiter.generation() == on.generation);
}

TEST_CASE("a request waiting without blocking keeps its place in line and is served whoever looks") {
    // A stream with replies on their way puts its next READ in line (enqueue) and reads them, looking
    // after each (ready). Others in line behind it don't wait for it to look: whoever waits serves
    // every request at the head whose tokens are there.
    rate::RateLimiter limiter;
    limiter.set_rate(MiB); // 128 KiB an eighth of a second, starting empty
    std::stop_source never;
    const auto start = Clock::now();
    const auto mine = limiter.enqueue(128 * KiB, 1, 2);
    REQUIRE(mine != 0);
    CHECK_FALSE(limiter.ready(mine)); // no tokens yet
    std::atomic<double> other_took{0};
    {
        // Another flow's request behind it, blocking: served after mine, at about a quarter second.
        std::jthread other([&] {
            CHECK(limiter.acquire(128 * KiB, never.get_token(), 2, 2));
            other_took = std::chrono::duration<double>(Clock::now() - start).count();
        });
    }
    CHECK(other_took.load() == doctest::Approx(0.25).epsilon(0.3));
    CHECK(limiter.ready(mine));       // served meanwhile, on its behalf
    CHECK_FALSE(limiter.ready(mine)); // and spent

    // With nothing on its way, a stream waits for its turn: about an eighth of a second each.
    const auto next = limiter.enqueue(128 * KiB, 1, 2);
    const auto from = Clock::now();
    CHECK(limiter.wait(next, never.get_token()));
    CHECK(std::chrono::duration<double>(Clock::now() - from).count() == doctest::Approx(0.125).epsilon(0.3));
}

TEST_CASE("a waiting request is given up at a new rate or a stop, and goes at once when the limit is lifted") {
    rate::RateLimiter limiter;
    limiter.set_rate(64 * KiB); // a second per 64 KiB
    std::stop_source never;
    // A new rate: the wait ends (false) so its stream looks again, its ticket given up.
    const auto t = limiter.enqueue(64 * KiB, 1, 2);
    std::atomic<int> result{-1};
    {
        std::jthread waiter([&] { result = limiter.wait(t, never.get_token()) ? 1 : 0; });
        std::this_thread::sleep_for(100ms);
        CHECK(result.load() == -1);
        limiter.set_rate(32 * KiB);
    }
    CHECK(result.load() == 0);
    CHECK_FALSE(limiter.ready(t)); // gone
    // A stop the same.
    const auto s = limiter.enqueue(32 * KiB, 1, 2);
    std::stop_source stop;
    {
        std::jthread waiter([&] { result = limiter.wait(s, stop.get_token()) ? 1 : 0; });
        std::this_thread::sleep_for(100ms);
        stop.request_stop();
    }
    CHECK(result.load() == 0);
    // Lifted: everyone in line goes at once, and nothing is put in line any more.
    const auto u = limiter.enqueue(32 * KiB, 1, 2);
    CHECK_FALSE(limiter.ready(u));
    limiter.set_rate(0);
    CHECK(limiter.ready(u));
    CHECK(limiter.enqueue(MiB, 1, 2) == 0);
    // A ticket given up lets the next in line go: no one waits behind a stream that left.
    limiter.set_rate(64 * KiB);
    const auto gone = limiter.enqueue(64 * KiB, 1, 2);
    const auto behind = limiter.enqueue(64 * KiB, 2, 2);
    std::this_thread::sleep_for(1100ms); // tokens for one
    limiter.cancel(gone);
    CHECK(limiter.ready(behind));
}
