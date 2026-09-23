#include "process.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace grab;

namespace {
std::string util_trim_right(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    return s;
}

// A shell one-liner runner for the host platform.
std::vector<std::string> shell(const char* script) {
#ifdef _WIN32
    return {"cmd.exe", "/d", "/c", script};
#else
    return {"sh", "-c", script};
#endif
}
} // namespace

TEST_CASE("run_capture returns stdout and the exit code") {
    auto r = proc::run_capture(shell("echo hello"));
    REQUIRE_MESSAGE(r.has_value(), r.error_or(""));
    CHECK(r->exit_code == 0);
    CHECK(r->out.find("hello") != std::string::npos);

    auto code = proc::run_capture(shell("exit 7"));
    REQUIRE(code.has_value());
    CHECK(code->exit_code == 7);
    CHECK(code->out.empty());
}

TEST_CASE("run_inherit returns the exit code") {
    auto r = proc::run_inherit(shell("exit 3"));
    REQUIRE_MESSAGE(r.has_value(), r.error_or(""));
    CHECK(*r == 3);
}

TEST_CASE("detached run_capture captures stderr separately and needs no console") {
    proc::RunOptions opts;
    opts.detached = true;
    auto r = proc::run_capture(shell("echo out& echo err 1>&2& exit 4"), opts);
    REQUIRE_MESSAGE(r.has_value(), r.error_or(""));
    CHECK(r->exit_code == 4);
    CHECK(r->out.find("out") != std::string::npos);
    CHECK(r->out.find("err") == std::string::npos);
    CHECK(r->err.find("err") != std::string::npos);
}

TEST_CASE("run_streaming delivers stderr line by line, CRLF stripped, stdout ignored") {
    std::vector<std::string> lines;
    auto r = proc::run_streaming(shell("echo one 1>&2& echo two 1>&2& echo ignored& exit 2"),
                                 [&](std::string_view l) { lines.emplace_back(l); });
    REQUIRE_MESSAGE(r.has_value(), r.error_or(""));
    CHECK(*r == 2);
    REQUIRE(lines.size() == 2);
    CHECK(util_trim_right(lines[0]) == "one");
    CHECK(util_trim_right(lines[1]) == "two");
    for (const auto& l : lines) CHECK(l.find('\r') == std::string::npos);
}

TEST_CASE("a stop request ends a long-running child and its children quickly") {
#ifdef _WIN32
    // cmd waits on ping, a grandchild: both must die with the job.
    const auto sleeper = shell("ping -n 30 127.0.0.1 >nul");
#else
    const auto sleeper = shell("sleep 30");
#endif
    std::stop_source stop;
    proc::RunOptions opts;
    opts.stop = stop.get_token();
    opts.detached = true;

    std::jthread canceller([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stop.request_stop();
    });
    const auto start = std::chrono::steady_clock::now();
    auto r = proc::run_streaming(sleeper, {}, opts);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE_MESSAGE(r.has_value(), r.error_or(""));
    CHECK(*r == proc::exit_stopped);
    CHECK(elapsed < std::chrono::seconds(5));

    // Same for run_capture.
    std::stop_source stop2;
    opts.stop = stop2.get_token();
    std::jthread canceller2([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        stop2.request_stop();
    });
    const auto start2 = std::chrono::steady_clock::now();
    auto c = proc::run_capture(sleeper, opts);
    REQUIRE(c.has_value());
    CHECK(c->exit_code == proc::exit_stopped);
    CHECK(std::chrono::steady_clock::now() - start2 < std::chrono::seconds(5));
}

TEST_CASE("run_capture feeds input to the child's stdin") {
#ifdef _WIN32
    const std::vector<std::string> echo_stdin{"findstr", "^"}; // prints every input line
#else
    const std::vector<std::string> echo_stdin{"cat"};
#endif
    proc::RunOptions opts;
    opts.input = std::string("secret-line\nsecond\n");
    for (const bool detached : {false, true}) {
        opts.detached = detached;
        auto r = proc::run_capture(echo_stdin, opts);
        REQUIRE_MESSAGE(r.has_value(), r.error_or(""));
        CHECK(r->exit_code == 0);
        CHECK(r->out.find("secret-line") != std::string::npos);
        CHECK(r->out.find("second") != std::string::npos);
    }
}

TEST_CASE("an already-requested stop still returns cleanly") {
    std::stop_source stop;
    stop.request_stop();
    proc::RunOptions opts;
    opts.stop = stop.get_token();
    auto r = proc::run_capture(shell("echo hi"), opts);
    REQUIRE(r.has_value()); // killed at once or finished first; either way no hang or crash
}

TEST_CASE("a missing executable is reported, not crashed on") {
    const std::vector<std::string> argv{"grab-definitely-not-installed-xyz", "--version"};
    auto r = proc::run_capture(argv);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().find("grab-definitely-not-installed-xyz") != std::string::npos);
    CHECK_FALSE(proc::run_inherit(argv).has_value());
}
