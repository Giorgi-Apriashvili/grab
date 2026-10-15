#include "progress_line.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>

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
