#include "activity.hpp"
#include "process.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace grab;

TEST_CASE("remembered step times") {
    CHECK(activity::blend(std::nullopt, 2000) == 2000);
    CHECK(activity::blend(1000, 2000) == 1600); // leans on the latest
    CHECK(activity::blend(5000, 1000) == 2600);
    CHECK(activity::blend(-5, 800) == 800);     // nonsense before: the new one alone
    CHECK(activity::blend(1000, 0) == 401);     // at least 1 ms
    CHECK(activity::blend(std::nullopt, 99'999'999) == activity::max_ms);
    CHECK(activity::guess("lsjson:Box.Guest") == 2000);
    CHECK(activity::guess("list:Main") == 10000);
    CHECK(activity::guess("search:Box") == 5000);
    CHECK(activity::guess("update") == 2000);
    CHECK(activity::guess("other") == 3000);
}

TEST_CASE("counting a listing's records as it streams") {
    activity::Records r;
    r.feed("a\nb\n");
    r.feed("c");
    CHECK(r.count() == 2);
    r.feed("\n");
    CHECK(r.count() == 3);
    r.feed(std::string("x\0y\0", 4)); // find -print0
    CHECK(r.count() == 5);
    activity::Records items('{'); // rclone lsjson
    items.feed("[\n{\"Path\":\"a {1}\"},\n{\"Pa");
    items.feed("th\":\"b\"}\n]\n");
    CHECK(items.count() == 2);
}

TEST_CASE("a child's output, seen while it runs") {
    std::vector<std::string> chunks;
    proc::RunOptions run;
    run.detached = true;
    run.on_output = [&](std::string_view s) { chunks.emplace_back(s); };
#ifdef _WIN32
    const std::vector<std::string> argv{"cmd.exe", "/c", "echo one& echo two"};
#else
    const std::vector<std::string> argv{"/bin/sh", "-c", "echo one; echo two"};
#endif
    const auto ran = proc::run_capture(argv, run);
    REQUIRE(ran.has_value());
    std::string seen;
    for (const auto& c : chunks) seen += c;
    CHECK(seen == ran->out); // everything captured was seen, in order
    CHECK(seen.find("one") != std::string::npos);
    CHECK(seen.find("two") != std::string::npos);
}
