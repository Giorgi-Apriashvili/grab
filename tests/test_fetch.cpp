#include "conn_budget.hpp"
#include "fetch.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace grab;
using namespace std::chrono_literals;

constexpr std::uint64_t MiB = std::uint64_t{1} << 20;

TEST_CASE("connection budgets: known limits, overrides, learned values") {
    CHECK(budget::is_storage_box("u443987-sub1.your-storagebox.de"));
    CHECK(budget::is_storage_box("U1.YOUR-STORAGEBOX.DE"));
    CHECK_FALSE(budget::is_storage_box("65.21.224.123"));
    CHECK(budget::default_budget("u1.your-storagebox.de") == 8);
    CHECK(budget::default_budget("example.com") == 12);
    CHECK(budget::effective_budget(std::nullopt, 12, std::nullopt) == 12);
    CHECK(budget::effective_budget(std::nullopt, 12, 7) == 7);
    CHECK(budget::effective_budget(std::nullopt, 8, 20) == 8); // learning only lowers
    CHECK(budget::effective_budget(16, 8, 3) == 16);            // an explicit setting wins
}

TEST_CASE("refusals are told apart from other failures") {
    using budget::Failure;
    CHECK(budget::classify_failure("Failed to create file system: couldn't connect SSH: ssh: handshake failed: EOF") ==
          Failure::refused);
    CHECK(budget::classify_failure("dial tcp 1.2.3.4:23: connectex: No connection could be made because the "
                                   "target machine actively refused it. connection refused") == Failure::refused);
    CHECK(budget::classify_failure("kex_exchange_identification: read: Connection reset by peer") == Failure::refused);
    CHECK(budget::classify_failure("object not found") == Failure::other);
    CHECK(budget::classify_failure("") == Failure::other);
}

TEST_CASE("a server pool shares its budget fairly and staggers starts") {
    budget::ServerPool pool(8);
    auto t = budget::ServerPool::Clock::time_point{} + 10s;
    pool.add_user(1);
    CHECK(pool.share(1) == 8);
    // A lone download takes the whole budget, one start per stagger gap.
    for (int i = 0; i < 8; ++i) {
        REQUIRE(pool.may_start(1, t));
        pool.started(1, t);
        CHECK_FALSE(pool.may_start(1, t)); // too soon after the last start
        t += budget::ServerPool::stagger;
    }
    CHECK_FALSE(pool.may_start(1, t)); // budget used up
    CHECK(pool.total_active() == 8);

    // A second download halves the shares; the first gives connections back as chunks end.
    pool.add_user(2);
    CHECK(pool.share(1) == 4);
    CHECK(pool.share(2) == 4);
    CHECK_FALSE(pool.may_start(2, t)); // all 8 still open
    pool.finished(1);
    CHECK(pool.may_start(2, t));

    // Three users of 8: 3 + 3 + 2, the oldest getting the remainder.
    pool.add_user(3);
    CHECK(pool.share(1) == 3);
    CHECK(pool.share(2) == 3);
    CHECK(pool.share(3) == 2);
    pool.remove_user(1);
    CHECK(pool.share(2) == 4);
    CHECK(pool.share(3) == 4);
}

TEST_CASE("bandwidth priority weights the connection shares 4:2:1") {
    budget::ServerPool pool(8);
    pool.add_user(1, 1); // low, oldest
    pool.add_user(2, 4); // high
    CHECK(pool.share(2) == 7); // 6.4 -> 6, plus the remainder: the heaviest first
    CHECK(pool.share(1) == 1);
    pool.add_user(3, 2); // normal
    // 8 x 4/7 = 4.57, 8 x 2/7 = 2.29, 8 x 1/7 = 1.14 -> 4 + 2 + 1, remainder 1 to high
    CHECK(pool.share(2) == 5);
    CHECK(pool.share(3) == 2);
    CHECK(pool.share(1) == 1);
    pool.set_weight(1, 4); // low raised to high: two highs split evenly, the older first
    CHECK(pool.share(1) == 4);
    CHECK(pool.share(2) == 3);
    CHECK(pool.share(3) == 1);
}

TEST_CASE("more downloads than connections: each still gets one") {
    budget::ServerPool pool(2);
    auto t = budget::ServerPool::Clock::time_point{} + 10s;
    for (int id = 1; id <= 3; ++id) pool.add_user(id);
    for (int id = 1; id <= 3; ++id) {
        CHECK(pool.share(id) == 1);
        REQUIRE(pool.may_start(id, t));
        pool.started(id, t);
        t += 1s;
    }
    CHECK(pool.total_active() == 3);
}

TEST_CASE("a refusal lowers the budget, never below 1, and only with two or more open") {
    budget::ServerPool pool(3);
    pool.add_user(1);
    auto t = budget::ServerPool::Clock::time_point{} + 10s;
    pool.started(1, t);
    CHECK_FALSE(pool.refused()); // a single connection failing says nothing about the limit
    pool.started(1, t + 1s);
    CHECK(pool.refused());
    CHECK(pool.budget() == 2);
    CHECK(pool.refused());
    CHECK(pool.budget() == 1);
    CHECK_FALSE(pool.refused());
    CHECK(pool.budget() == 1);
}

TEST_CASE("unused share is borrowed, and handed back when its owner waits") {
    budget::ServerPool pool(8);
    auto t = budget::ServerPool::Clock::time_point{} + 10s;
    pool.add_user(1); // a big file
    pool.add_user(2); // a small file that can only use one connection
    for (int i = 0; i < 4; ++i) {
        pool.started(1, t);
        t += 1s;
    }
    pool.started(2, t);
    t += 1s;
    // User 1 is at its share (4), but user 2 isn't asking for more: borrow the idle capacity.
    CHECK(pool.may_start(1, t));
    pool.started(1, t);
    t += 1s;
    CHECK_FALSE(pool.should_yield(1));
    // Once user 2 waits below its share, user 1 stops borrowing and gives one back.
    pool.waiting(2, +1);
    CHECK_FALSE(pool.may_start(1, t));
    CHECK(pool.should_yield(1));
    pool.finished(1);
    CHECK_FALSE(pool.should_yield(1)); // back at its share
    CHECK(pool.may_start(2, t));
}

TEST_CASE("a download over its share hands a connection to one that is waiting") {
    fetch::Connections conns;
    conns.configure("box", 4);
    conns.join("box", 1);
    std::stop_source never;
    for (int i = 0; i < 4; ++i) REQUIRE(conns.acquire("box", 1, never.get_token())); // staggered starts
    CHECK_FALSE(conns.yield_if_over_share("box", 1)); // alone: keeps them all
    conns.join("box", 2);
    CHECK_FALSE(conns.yield_if_over_share("box", 1)); // joined, but not asking yet
    std::atomic<bool> got{false};
    std::jthread waiter([&] { got = conns.acquire("box", 2, never.get_token()); });
    for (int i = 0; i < 100 && !conns.yield_if_over_share("box", 1); ++i) std::this_thread::sleep_for(10ms);
    waiter.join();
    CHECK(got.load());
    CHECK(conns.active("box", 1) == 3);
    CHECK(conns.active("box", 2) == 1);
    // A stop ends a wait for a connection that will never come.
    std::stop_source stop;
    stop.request_stop();
    CHECK_FALSE(conns.acquire("box", 2, stop.get_token()));
}

TEST_CASE("chunk sizes keep every stream busy within limits") {
    CHECK(fetch::chunk_size_for(10 * MiB, 8) == fetch::min_chunk);        // small file: one 16 MiB chunk
    CHECK(fetch::chunk_size_for(400 * MiB, 8) == 50 * MiB);               // 8 chunks for 8 streams
    CHECK(fetch::chunk_size_for(20000 * MiB, 8) == fetch::max_chunk);     // big file: capped
    CHECK(fetch::chunk_size_for(400 * MiB, 0) == fetch::max_chunk);       // treated as one stream
}

TEST_CASE("download state: ranges, progress and JSON round trip") {
    auto s = fetch::new_state("hetzner", "/home/x/movie.mkv", 100 * MiB + 5, "2023-10-15T11:22:33Z", 32 * MiB);
    REQUIRE(s.ranges.size() == 4);
    CHECK(s.ranges[3] == fetch::Range{96 * MiB, 4 * MiB + 5, 0});
    CHECK_FALSE(s.complete());
    s.ranges[0].done = 32 * MiB;
    s.ranges[1].done = 7;
    s.ranges[3].done = 4 * MiB + 5;
    CHECK(s.bytes_done() == 36 * MiB + 12);
    auto back = fetch::state_from_json(fetch::state_to_json(s));
    REQUIRE(back.has_value());
    CHECK(*back == s);
    for (auto& r : s.ranges) r.done = r.length;
    CHECK(s.complete());

    // Inconsistent state is refused rather than trusted.
    CHECK_FALSE(fetch::state_from_json("{}").has_value());
    CHECK_FALSE(fetch::state_from_json("not json").has_value());
    auto gap = s;
    gap.ranges.erase(gap.ranges.begin() + 1); // the file is no longer covered
    CHECK_FALSE(fetch::state_from_json(fetch::state_to_json(gap)).has_value());
    auto over = s;
    over.ranges[3].done = over.ranges[3].length + 1; // more than the range holds
    CHECK_FALSE(fetch::state_from_json(fetch::state_to_json(over)).has_value());
}

TEST_CASE("splitting a busy range hands its second half to an idle stream") {
    auto s = fetch::new_state("box", "/m.mkv", 64 * MiB, "2025-01-01T00:00:00Z", 64 * MiB);
    s.ranges[0].done = 10 * MiB;
    auto tail = fetch::split(s, 0);
    REQUIRE(tail.has_value());
    REQUIRE(s.ranges.size() == 2);
    const auto& a = s.ranges[0];
    const auto& b = s.ranges[*tail];
    CHECK(a.start + a.length == b.start); // adjacent, nothing lost or doubled
    CHECK(a.length + b.length == 64 * MiB);
    CHECK(b.done == 0);
    CHECK(b.length == 27 * MiB); // half of the 54 MiB still to fetch
    CHECK(b.start % (64 * 1024) == 0);
    // The split state still tiles the file and survives saving.
    auto back = fetch::state_from_json(fetch::state_to_json(s));
    REQUIRE(back.has_value());
    CHECK(*back == s);
    // Too little left to be worth another connection.
    auto small = fetch::new_state("box", "/m.mkv", 6 * MiB, "2025-01-01T00:00:00Z", 6 * MiB);
    CHECK_FALSE(fetch::split(small, 0).has_value());
    CHECK(small.ranges.size() == 1);
}

TEST_CASE("rclone lsjson --stat and RFC 3339 times") {
    auto st = fetch::parse_stat(R"({"Path":"a.mkv","Name":"a.mkv","Size":9165966961,"MimeType":"video/x-matroska",)"
                                R"("ModTime":"2025-11-02T14:03:05.123456789+01:00","IsDir":false})");
    REQUIRE(st.has_value());
    CHECK(st->size == 9165966961ULL);
    CHECK(st->modtime == "2025-11-02T14:03:05.123456789+01:00");
    CHECK_FALSE(st->is_dir);
    CHECK_FALSE(fetch::parse_stat("[]").has_value());

    CHECK(fetch::parse_rfc3339("1970-01-01T00:00:00Z") == 0);
    CHECK(fetch::parse_rfc3339("2000-01-01T00:00:00Z") == std::int64_t{946684800} * 1'000'000'000);
    CHECK(fetch::parse_rfc3339("2000-01-01T01:00:00+01:00") == std::int64_t{946684800} * 1'000'000'000);
    CHECK(fetch::parse_rfc3339("2000-01-01T00:00:00.5Z") == std::int64_t{946684800} * 1'000'000'000 + 500'000'000);
    CHECK_FALSE(fetch::parse_rfc3339("2000-01-01 garbage").has_value());
    CHECK_FALSE(fetch::parse_rfc3339("2000-01-01T00:00:00").has_value()); // no zone
}

TEST_CASE("rclone command lines for ranges") {
    fetch::Source src{"rclone.exe", util::path_from_utf8("C:\\g\\rclone.conf"), "box", "/home/a b.mkv",
                      {"--sftp-chunk-size", "255Ki"}};
    const auto cat = fetch::cat_argv(src, 1000, 2000);
    CHECK(cat[0] == "rclone.exe");
    CHECK(cat[1] == "cat");
    CHECK(cat[2] == "box:/home/a b.mkv");
    auto has_pair = [&](const std::vector<std::string>& v, std::string_view a, std::string_view b) {
        for (std::size_t i = 0; i + 1 < v.size(); ++i) {
            if (v[i] == a && v[i + 1] == b) return true;
        }
        return false;
    };
    CHECK(has_pair(cat, "--offset", "1000"));
    CHECK(has_pair(cat, "--count", "2000"));
    CHECK(has_pair(cat, "--config", "C:\\g\\rclone.conf"));
    CHECK(has_pair(cat, "--sftp-chunk-size", "255Ki"));
    CHECK_FALSE(has_pair(cat, "--buffer-size", "0"));
    const auto limited = fetch::cat_argv(src, 1000, 2000, true); // under a speed limit: no read-ahead
    CHECK(has_pair(limited, "--buffer-size", "0"));
    CHECK(has_pair(limited, "--sftp-concurrency", "4"));
    const auto stat = fetch::stat_argv(src);
    CHECK(stat[1] == "lsjson");
    CHECK(stat[2] == "--stat");
}

TEST_CASE("saved progress and discard") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-fetch";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto target = dir / "movie.mkv";
    CHECK_FALSE(fetch::saved_progress(target).has_value());
    auto s = fetch::new_state("box", "/m.mkv", 40 * MiB, "2025-01-01T00:00:00Z", 16 * MiB);
    s.ranges[0].done = 16 * MiB;
    s.ranges[1].done = 3;
    std::ofstream(fetch::state_path(target)) << fetch::state_to_json(s);
    std::ofstream(fetch::part_path(target)) << "x";
    auto p = fetch::saved_progress(target);
    REQUIRE(p.has_value());
    CHECK(p->first == 16 * MiB + 3);
    CHECK(p->second == 40 * MiB);
    fetch::discard(target);
    CHECK_FALSE(std::filesystem::exists(fetch::state_path(target)));
    CHECK_FALSE(std::filesystem::exists(fetch::part_path(target)));
    std::filesystem::remove_all(dir);
}
