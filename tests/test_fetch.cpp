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
    CHECK(budget::default_budget("u1.your-storagebox.de") == 14);
    CHECK(budget::default_budget("example.com") == 12);
    CHECK(budget::effective_budget(std::nullopt, 12, std::nullopt) == 12);
    CHECK(budget::effective_budget(std::nullopt, 12, 7) == 7);
    CHECK(budget::effective_budget(std::nullopt, 8, 20) == 8); // learning only lowers
    CHECK(budget::effective_budget(16, 8, 3) == 16);            // an explicit setting wins
    // A learned value creeps back up after clean downloads, and is dropped at the default (G10).
    CHECK(budget::raised(9, 14) == 10);
    CHECK_FALSE(budget::raised(13, 14).has_value());
    CHECK_FALSE(budget::raised(20, 14).has_value());
}

TEST_CASE("refusals are told apart from other failures") {
    using budget::Failure;
    CHECK(budget::classify_failure("Failed to create file system: couldn't connect SSH: ssh: handshake failed: EOF") ==
          Failure::refused);
    CHECK(budget::classify_failure("kex_exchange_identification: read: Connection reset by peer") == Failure::refused);
    CHECK(budget::classify_failure("object not found") == Failure::other);
    CHECK(budget::classify_failure("") == Failure::other);

    // As rclone 1.75.1 printed them against a real server (G9): the Windows text for a refused
    // port has no "connection refused" in it...
    CHECK(budget::classify_failure("NewFs: couldn't connect SSH: dial tcp 127.0.0.1:1: connectex: No connection could be "
                                   "made because the target machine actively refused it.") == Failure::refused);
    // ...and a refused login or a changed host key, though "handshake failed" too, stop the download.
    CHECK(budget::classify_failure("NewFs: couldn't connect SSH: ssh: handshake failed: ssh: unable to authenticate, "
                                   "attempted methods [none publickey], no supported methods remain") == Failure::fatal);
    CHECK(budget::classify_failure("NewFs: couldn't connect SSH: ssh: handshake failed: knownhosts: key mismatch") ==
          Failure::fatal);
    CHECK(budget::classify_failure("ssh: handshake failed: ssh: disconnect, reason 2: Too many authentication failures") ==
          Failure::fatal);
    CHECK(budget::classify_failure("ssh: disconnect, reason 11: too many connections") == Failure::refused);
    CHECK(budget::classify_failure("read tcp: wsarecv: A connection attempt failed because the connected party did not "
                                   "properly respond") == Failure::other);
}

#ifdef _WIN32
TEST_CASE("a download whose logins are refused for good stops with the reason, at once (G9)") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-fatal";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    // A stand-in rclone: the size check works, every read is refused as after a host key change.
    std::ofstream(dir / "rclone.cmd", std::ios::binary)
        << "@echo off\r\n"
           "if \"%2\"==\"--stat\" (\r\n"
           "  echo {\"Path\":\"f.bin\",\"Name\":\"f.bin\",\"Size\":67108864,\"ModTime\":\"2026-01-01T00:00:00Z\",\"IsDir\":false}\r\n"
           "  exit /b 0\r\n"
           ")\r\n"
           "echo CRITICAL: Failed to create file system: NewFs: couldn't connect SSH: ssh: handshake failed: knownhosts: key mismatch 1>&2\r\n"
           "exit /b 1\r\n";
    std::vector<int> learned;
    fetch::Connections connections([&](const std::string&, int b) { learned.push_back(b); });
    connections.configure("Main", 12);
    fetch::Download d;
    d.source = fetch::Source{.rclone = util::path_to_utf8(dir / "rclone.cmd"), .config = std::nullopt,
                             .rclone_remote = "Main", .path = "f.bin", .flags = {}};
    d.server = "Main";
    d.id = 1;
    d.target = dir / "f.bin";
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = fetch::run(d, connections, {}, {});
    CHECK(r.outcome == fetch::Outcome::failed);
    CHECK(r.error.find("knownhosts: key mismatch") != std::string::npos);
    CHECK(std::chrono::steady_clock::now() - t0 < 30s);
    CHECK(learned.empty()); // not taken for a full server
    std::filesystem::remove_all(dir);
}
#endif

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

TEST_CASE("more downloads than connections: never past the budget, and the one without waits first (G194)") {
    budget::ServerPool pool(2);
    auto t = budget::ServerPool::Clock::time_point{} + 10s;
    for (int id = 1; id <= 3; ++id) pool.add_user(id);
    for (int id = 1; id <= 2; ++id) {
        REQUIRE(pool.may_start(id, t));
        pool.started(id, t);
        t += 1s;
    }
    CHECK_FALSE(pool.may_start(3, t)); // the budget is a hard cap
    pool.waiting(3, +1);
    CHECK(pool.total_active() == 2);
    pool.finished(1);
    CHECK_FALSE(pool.may_start(2, t)); // 2 has one, 3 has none and waits: 3 goes first
    CHECK(pool.may_start(3, t));
    pool.waiting(3, -1);
    pool.started(3, t);
    CHECK(pool.total_active() == 2);
}

TEST_CASE("a refusal lowers the budget, never below 1, and only while another connection gets data (G10)") {
    budget::ServerPool pool(3);
    pool.add_user(1);
    auto t = budget::ServerPool::Clock::time_point{} + 10s;
    pool.started(1, t);
    CHECK_FALSE(pool.refused()); // a single connection failing says nothing about the limit
    pool.started(1, t + 1s);
    pool.started(1, t + 2s);
    // Several open but none getting data: the server is full of something else's connections.
    CHECK_FALSE(pool.refused());
    CHECK(pool.budget() == 3);
    pool.working(1, +1); // one gets data: the others are over the server's limit
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

TEST_CASE("rclone lsjson --stat") {
    auto st = fetch::parse_stat(R"({"Path":"a.mkv","Name":"a.mkv","Size":9165966961,"MimeType":"video/x-matroska",)"
                                R"("ModTime":"2025-11-02T14:03:05.123456789+01:00","IsDir":false})");
    REQUIRE(st.has_value());
    CHECK(st->size == 9165966961ULL);
    CHECK(st->modtime == "2025-11-02T14:03:05.123456789+01:00");
    CHECK_FALSE(st->is_dir);
    CHECK_FALSE(fetch::parse_stat("[]").has_value());
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

namespace {

void write(const std::filesystem::path& p, const std::string& text) { std::ofstream(p, std::ios::binary) << text; }

std::string read(const std::filesystem::path& p) { return util::read_file(p).value_or("<missing>"); }

void sized(const std::filesystem::path& p, std::uint64_t size) {
    std::ofstream(p, std::ios::binary).close();
    std::filesystem::resize_file(p, size);
}

} // namespace

TEST_CASE("partial download names: slot 1 is the plain one") {
    const std::filesystem::path target = util::path_from_utf8("D:\\dl\\movie.mkv");
    CHECK(fetch::partial_files(target, 1).part == util::path_from_utf8("D:\\dl\\movie.mkv.grabpart"));
    CHECK(fetch::partial_files(target, 1).record == util::path_from_utf8("D:\\dl\\movie.mkv.grabpart.json"));
    CHECK(fetch::partial_files(target, 2).part == util::path_from_utf8("D:\\dl\\movie.mkv.2.grabpart"));
    CHECK(fetch::partial_files(target, 9).record == util::path_from_utf8("D:\\dl\\movie.mkv.9.grabpart.json"));
}

TEST_CASE("grab's partial download: found by its record, discarded with nothing else") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-fetch";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto target = dir / "movie.mkv";
    CHECK_FALSE(fetch::saved_progress(target, "/m.mkv").has_value());
    auto s = fetch::new_state("box", "/m.mkv", 40 * MiB, "2025-01-01T00:00:00Z", 16 * MiB);
    s.ranges[0].done = 16 * MiB;
    s.ranges[1].done = 3;
    const auto one = fetch::partial_files(target, 1);
    write(one.record, fetch::state_to_json(s)); // a record as 0.15 wrote it: still grab's
    sized(one.part, 40 * MiB);
    auto p = fetch::saved_progress(target, "/m.mkv");
    REQUIRE(p.has_value());
    CHECK(p->first == 16 * MiB + 3);
    CHECK(p->second == 40 * MiB);
    CHECK_FALSE(fetch::saved_progress(target, "/other.mkv").has_value()); // another server file's
    fetch::discard(target, "/other.mkv");
    CHECK(std::filesystem::exists(one.record));
    fetch::discard(target, "/m.mkv");
    CHECK_FALSE(std::filesystem::exists(one.record));
    CHECK_FALSE(std::filesystem::exists(one.part));

    // The data file isn't grab's size any more: only the record goes.
    write(one.record, fetch::state_to_json(s));
    write(one.part, "the user's own file");
    fetch::discard(target, "/m.mkv");
    CHECK_FALSE(std::filesystem::exists(one.record));
    CHECK(read(one.part) == "the user's own file");
    std::filesystem::remove_all(dir);
}

TEST_CASE("files at grab's partial names that aren't grab's are never touched") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-fetch-foreign";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto target = dir / "normal.bin";
    const auto one = fetch::partial_files(target, 1);
    // The user's two files: no record, and a "record" that isn't one.
    write(one.part, "photos");
    write(one.record, "{\"notes\": true}");
    CHECK_FALSE(fetch::find_partial(target, "/normal.bin").has_value());
    fetch::discard(target, "/normal.bin");
    CHECK(read(one.part) == "photos");
    CHECK(read(one.record) == "{\"notes\": true}");
    // A new download takes the next free names.
    auto free = fetch::free_partial(target);
    REQUIRE(free.has_value());
    CHECK(*free == fetch::partial_files(target, 2));
    // A real record in slot 2 is found there, past the user's files.
    const auto s = fetch::new_state("main", "/normal.bin", 10, "2025-01-01T00:00:00Z", 16 * MiB);
    write(free->record, fetch::state_to_json(s));
    const auto found = fetch::find_partial(target, "/normal.bin");
    REQUIRE(found.has_value());
    CHECK(found->files == fetch::partial_files(target, 2));
    CHECK(*fetch::free_partial(target) == fetch::partial_files(target, 3));
    // Only the data file there is enough to make a slot taken.
    write(fetch::partial_files(target, 3).part, "x");
    CHECK(*fetch::free_partial(target) == fetch::partial_files(target, 4));
    // Every slot taken: none left.
    for (int slot = 4; slot <= fetch::max_slot; ++slot) write(fetch::partial_files(target, slot).part, "x");
    CHECK_FALSE(fetch::free_partial(target).has_value());
    std::filesystem::remove_all(dir);
}

TEST_CASE("reserved names: a folder's own files are never taken or read as grab's") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-fetch-reserved";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto target = dir / "a.bin";
    // The server folder holds "a.bin.grabpart.json", with a record naming a.bin in it.
    const auto one = fetch::partial_files(target, 1);
    const auto s = fetch::new_state("main", "/f/a.bin", 10, "2025-01-01T00:00:00Z", 16 * MiB);
    write(one.record, fetch::state_to_json(s));
    const fetch::Reserved reserved = [&](const std::filesystem::path& p) { return p == one.record; };
    CHECK(fetch::find_partial(target, "/f/a.bin").has_value());
    CHECK_FALSE(fetch::find_partial(target, "/f/a.bin", reserved).has_value());
    fetch::discard(target, "/f/a.bin", reserved);
    CHECK(std::filesystem::exists(one.record));
    std::filesystem::remove(one.record);
    CHECK(*fetch::free_partial(target, reserved) == fetch::partial_files(target, 2)); // even with nothing there
    std::filesystem::remove_all(dir);
}

TEST_CASE("a local copy is up to date with the same size and a time not older than the server's") {
    constexpr std::int64_t sec = 1'000'000'000;
    const std::int64_t server = std::int64_t{1'700'000'000} * sec;
    CHECK(fetch::up_to_date(100, server, 100, server));                // what grab and rclone leave
    CHECK(fetch::up_to_date(100, server + 3600 * sec, 100, server));   // copied after the server's last change
    CHECK(fetch::up_to_date(100, server - 1 * sec, 100, server));      // within the slack of coarse file systems
    CHECK_FALSE(fetch::up_to_date(100, server - 60 * sec, 100, server)); // the server's file changed since
    CHECK_FALSE(fetch::up_to_date(99, server, 100, server));
    CHECK(fetch::up_to_date(100, 0, 100, std::nullopt)); // no server time: the size decides

    const auto dir = std::filesystem::temp_directory_path() / "grab-test-uptodate";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto file = dir / "a.bin";
    std::ofstream(file, std::ios::binary) << "12345"; // written now
    CHECK(fetch::up_to_date(file, 5, "2000-01-01T00:00:00Z"));
    CHECK_FALSE(fetch::up_to_date(file, 5, "2100-01-01T00:00:00Z")); // older than the server's copy
    CHECK_FALSE(fetch::up_to_date(file, 6, "2000-01-01T00:00:00Z"));
    CHECK(fetch::up_to_date(file, 5, "")); // unknown server time
    CHECK_FALSE(fetch::up_to_date(dir / "missing.bin", 5, ""));
    CHECK_FALSE(fetch::up_to_date(dir, 0, "")); // a directory is not a copy of a file
    std::filesystem::remove_all(dir);
}

TEST_CASE("a folder's batch learns when to take connections back or give them (G192)") {
    fetch::Connections c;
    c.configure("box", 14);
    c.join("box", 1);
    const int held = c.reserve("box", 1, {});
    CHECK(held == 14);
    CHECK_FALSE(c.unbalanced("box", 1, held)); // alone: all of them
    c.join("box", 2);
    std::stop_source give_up;
    std::jthread file([&](std::stop_token) { (void)c.acquire("box", 2, give_up.get_token()); });
    std::this_thread::sleep_for(100ms); // the file now waits for its share
    CHECK(c.unbalanced("box", 1, held)); // so the batch starts again with fewer
    give_up.request_stop();
    file.join();
    c.release("box", 1, held);

    // A batch that started with one while another download held the rest grows when it ends.
    fetch::Connections d;
    d.configure("box", 14);
    d.join("box", 1);
    d.join("box", 2);
    for (int i = 0; i < 13; ++i) REQUIRE(d.acquire("box", 2, {}));
    const int small_batch = d.reserve("box", 1, {});
    CHECK(small_batch == 1);
    CHECK_FALSE(d.unbalanced("box", 1, small_batch));
    d.release("box", 2, 13);
    d.leave("box", 2);
    CHECK(d.unbalanced("box", 1, small_batch));
}

#ifdef _WIN32
TEST_CASE("a size check refused by a full server is asked again, not failed (G193)") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-stat-refused";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    // The first size check is refused, the second answers; every read then fails for good, which
    // ends the download with that read's message rather than the size check's.
    std::ofstream(dir / "rclone.cmd", std::ios::binary)
        << "@echo off\r\n" // (labels: an exit /b inside nested parentheses comes back as 0)
           "if not \"%2\"==\"--stat\" goto read\r\n"
           "if exist \"%~dp0asked\" goto answer\r\n"
           "echo x> \"%~dp0asked\"\r\n"
           "echo CRITICAL: couldn't connect SSH: ssh: disconnect, reason 11: too many connections 1>&2\r\n"
           "exit /b 1\r\n"
           ":answer\r\n"
           "echo {\"Path\":\"f.bin\",\"Name\":\"f.bin\",\"Size\":67108864,\"ModTime\":\"2026-01-01T00:00:00Z\",\"IsDir\":false}\r\n"
           "exit /b 0\r\n"
           ":read\r\n"
           "echo CRITICAL: couldn't connect SSH: ssh: handshake failed: knownhosts: key mismatch 1>&2\r\n"
           "exit /b 1\r\n";
    fetch::Connections connections;
    connections.configure("Main", 12);
    fetch::Download d;
    d.source = fetch::Source{.rclone = util::path_to_utf8(dir / "rclone.cmd"), .config = std::nullopt,
                             .rclone_remote = "Main", .path = "f.bin", .flags = {}};
    d.server = "Main";
    d.id = 1;
    d.target = dir / "f.bin";
    const auto r = fetch::run(d, connections, {}, {});
    CHECK(r.outcome == fetch::Outcome::failed);
    CHECK_MESSAGE(r.error.find("knownhosts: key mismatch") != std::string::npos, r.error); // got past the size check
    CHECK(std::filesystem::exists(dir / "asked"));
    std::filesystem::remove_all(dir);
}
#endif
TEST_CASE("a listing takes its connections from the pool: up to 8 alone, what's free next to a download (G191)") {
    fetch::Connections c;
    c.configure("box", 14);
    {
        const fetch::Listing alone(c, "box", {});
        CHECK(alone.connections() == 8);
        CHECK(alone.flags() == std::vector<std::string>{"--checkers", "8", "--sftp-connections", "8"});
    }
    // A download holds 12 of 14 and doesn't give any back here: the listing takes the 2 left
    // (after waiting a moment for more), never going past the budget.
    c.join("box", 1);
    for (int i = 0; i < 12; ++i) REQUIRE(c.acquire("box", 1, {}));
    const fetch::Listing busy(c, "box", {});
    CHECK(busy.connections() == 2);
    CHECK(c.active("box", 1) == 12);
}
TEST_CASE("a partial download belongs to its server: the same path from another is left alone (G20)") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-fetch-servers";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto target = dir / "big.bin";
    const auto one = fetch::partial_files(target, 1);
    auto s = fetch::new_state("Main", "/tmp/big.bin", 40 * MiB, "2025-01-01T00:00:00Z", 16 * MiB);
    s.ranges[0].done = 16 * MiB;
    write(one.record, fetch::state_to_json(s));
    sized(one.part, 40 * MiB);
    CHECK(fetch::saved_progress(target, "/tmp/big.bin", {}, "Main").has_value());
    CHECK_FALSE(fetch::saved_progress(target, "/tmp/big.bin", {}, "Main2").has_value());
    fetch::discard(target, "/tmp/big.bin", {}, "Main2"); // Main2's: there is none
    CHECK(std::filesystem::exists(one.record));
    CHECK(std::filesystem::exists(one.part));
    // Without a server it is anyone's, as before.
    CHECK(fetch::saved_progress(target, "/tmp/big.bin").has_value());
    std::filesystem::remove_all(dir);
}

TEST_CASE("a partial download moves with Keep both to the second copy's name (G74)") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-fetch-move";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto target = dir / "big.bin";
    const auto second = dir / "big (2).bin";
    const auto one = fetch::partial_files(target, 1);
    auto s = fetch::new_state("Main", "/tmp/big.bin", 40 * MiB, "2025-01-01T00:00:00Z", 16 * MiB);
    s.ranges[0].done = 16 * MiB;
    write(one.record, fetch::state_to_json(s));
    sized(one.part, 40 * MiB);
    CHECK_FALSE(fetch::move_partial(target, second, "/tmp/big.bin", "Main2")); // another server's
    REQUIRE(fetch::move_partial(target, second, "/tmp/big.bin", "Main"));
    CHECK_FALSE(std::filesystem::exists(one.part));
    CHECK_FALSE(std::filesystem::exists(one.record));
    const auto p = fetch::saved_progress(second, "/tmp/big.bin", {}, "Main");
    CHECK(p.value_or(std::pair<std::uint64_t, std::uint64_t>{}).first == 16 * MiB);
    CHECK_FALSE(fetch::move_partial(target, second, "/tmp/big.bin", "Main")); // moved already
    std::filesystem::remove_all(dir);
}

#ifdef _WIN32
TEST_CASE("a file that already is the server's wins over a partial download of it (G75)") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-fetch-present";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    // The server's file: 64 MiB from 2026-01-01; the stand-in answers only the size check.
    std::ofstream(dir / "rclone.cmd", std::ios::binary)
        << "@echo off\r\n"
           "echo {\"Path\":\"f.bin\",\"Name\":\"f.bin\",\"Size\":67108864,\"ModTime\":\"2026-01-01T00:00:00Z\",\"IsDir\":false}\r\n";
    const auto target = dir / "f.bin";
    const auto one = fetch::partial_files(target, 1);
    auto s = fetch::new_state("Main", "/f.bin", 64 * MiB, "2026-01-01T00:00:00Z", 16 * MiB);
    s.ranges[0].done = 16 * MiB;
    write(one.record, fetch::state_to_json(s));
    sized(one.part, 64 * MiB);
    sized(target, 64 * MiB); // the same size, written now: newer than the server's
    fetch::Connections connections;
    connections.configure("Main", 12);
    fetch::Download d;
    d.source = fetch::Source{.rclone = util::path_to_utf8(dir / "rclone.cmd"), .config = std::nullopt,
                             .rclone_remote = "Main", .path = "/f.bin", .flags = {}};
    d.server = "Main";
    d.id = 1;
    d.target = target;
    const auto r = fetch::run(d, connections, {}, {});
    CHECK(r.outcome == fetch::Outcome::done);
    CHECK(r.present);
    CHECK_FALSE(std::filesystem::exists(one.record)); // given up
    CHECK(std::filesystem::file_size(target) == 64 * MiB);
    std::filesystem::remove_all(dir);
}
#endif