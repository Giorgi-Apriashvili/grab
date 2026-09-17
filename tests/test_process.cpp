#include "process.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace grab;

namespace {
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

TEST_CASE("a missing executable is reported, not crashed on") {
    const std::vector<std::string> argv{"grab-definitely-not-installed-xyz", "--version"};
    auto r = proc::run_capture(argv);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().find("grab-definitely-not-installed-xyz") != std::string::npos);
    CHECK_FALSE(proc::run_inherit(argv).has_value());
}
