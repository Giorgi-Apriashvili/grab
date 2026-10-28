#include "process.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (SetEnvironmentVariableW)
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
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

#ifdef _WIN32
TEST_CASE("run_inherit: grandchildren die with the child unless released") {
    // The child starts a grandchild (start /b) that writes a file about a second later, then
    // exits at once. An installer relaunching grab-gui is such a grandchild.
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-descendants";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    auto run = [&](const char* name, bool contain) {
        const auto file = dir / name;
        // ^-escaped so the inner cmd, not this one, runs the redirections and the &. The temp
        // path has no spaces (short names on CI), so no quotes are needed.
        const std::string script =
            "start /b cmd.exe /d /c ping -n 2 127.0.0.1 ^>nul ^& echo x^>" + file.string();
        auto r = proc::run_inherit(shell(script.c_str()), contain);
        REQUIRE_MESSAGE(r.has_value(), r.error_or(""));
        std::this_thread::sleep_for(std::chrono::milliseconds(2500));
        return std::filesystem::exists(file);
    };
    CHECK_FALSE(run("contained.txt", true));
    CHECK(run("released.txt", false));
    std::filesystem::remove_all(dir);
}
#endif

TEST_CASE("run_piped streams stdout in chunks, captures stderr, and can stop early") {
    // ~200 KB through the pipe, then a stderr line.
#ifdef _WIN32
    const char* script = "for /L %i in (1,1,2000) do @echo 0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789& echo done 1>&2";
#else
    const char* script = "i=0; while [ $i -lt 2000 ]; do echo 0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789; i=$((i+1)); done; echo done 1>&2";
#endif
    std::size_t bytes = 0;
    std::size_t lines = 0;
    auto r = proc::run_piped(shell(script), [&](std::string_view data) {
        bytes += data.size();
        lines += static_cast<std::size_t>(std::count(data.begin(), data.end(), '\n'));
        return true;
    });
    REQUIRE_MESSAGE(r.has_value(), r.error_or(""));
    CHECK(r->exit_code == 0);
    CHECK(lines == 2000);
    CHECK(bytes >= 2000 * 101);
    CHECK(r->err.find("done") != std::string::npos);

    // Returning false ends the child at once.
    std::size_t first = 0;
    auto stopped = proc::run_piped(shell(script), [&](std::string_view data) {
        first += data.size();
        return false;
    });
    REQUIRE(stopped.has_value());
    CHECK(stopped->exit_code == proc::exit_stopped);
    CHECK(first > 0);
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

#ifdef _WIN32
TEST_CASE("programs started by bare name never come from the current folder (G3)") {
    namespace fs = std::filesystem;
    const auto dir = fs::temp_directory_path() / "grab-find-program-test";
    fs::remove_all(dir);
    fs::create_directories(dir / "onpath");
    // Planted where grab runs from (a download folder): names grab starts by bare name. They
    // aren't even programs, so starting one would fail.
    for (const char* name : {"cmd.exe", "whoami.exe", "nothere-xyz.exe"}) std::ofstream(dir / name) << "planted";
    std::ofstream(dir / "onpath" / "tool-xyz.cmd") << "@echo off";

    struct Restore {
        fs::path cwd = fs::current_path();
        std::wstring path = util::to_wide(util::getenv_utf8("PATH").value_or(""));
        fs::path dir;
        ~Restore() {
            SetEnvironmentVariableW(L"PATH", path.c_str());
            fs::current_path(cwd);
            std::error_code ec;
            fs::remove_all(dir, ec);
        }
    } restore{.dir = dir};
    fs::current_path(dir);
    // Relative PATH entries point at the current folder too.
    SetEnvironmentVariableW(L"PATH", (L".;bin;" + (dir / "onpath").wstring() + L";" + restore.path).c_str());

    const auto cmd = proc::find_program("cmd");
    REQUIRE_MESSAGE(cmd.has_value(), cmd.error_or(""));
    CHECK(cmd->parent_path() != dir);
    CHECK(util::to_lower(util::path_to_utf8(cmd->filename())) == "cmd.exe");
    const auto who = proc::find_program("whoami");
    REQUIRE(who.has_value());
    CHECK(who->parent_path() != dir);
    const auto missing = proc::find_program("nothere-xyz"); // only in the current folder and "."
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error() == "cannot find nothere-xyz.exe in System32 or PATH");
    // .cmd only when asked, as ShellExecute would (the window's editor), from an absolute entry.
    CHECK_FALSE(proc::find_program("tool-xyz").has_value());
    const auto tool = proc::find_program("tool-xyz", true);
    REQUIRE(tool.has_value());
    CHECK(util::to_lower(util::path_to_utf8(*tool)) == util::to_lower(util::path_to_utf8(dir / "onpath" / "tool-xyz.cmd")));
    // A path is the configuration's choice, taken as given.
    CHECK(proc::find_program("C:\\Tools\\rclone.exe").value() == fs::path(L"C:\\Tools\\rclone.exe"));
    CHECK(proc::find_program(".\\local.exe").value() == fs::path(L".\\local.exe"));

    // What runs is the real cmd.exe.
    const auto ran = proc::run_capture(std::vector<std::string>{"cmd", "/d", "/c", "echo real"});
    REQUIRE_MESSAGE(ran.has_value(), ran.error_or(""));
    CHECK(ran->out.find("real") != std::string::npos);
}
#endif

TEST_CASE("a missing executable is reported, not crashed on") {
    const std::vector<std::string> argv{"grab-definitely-not-installed-xyz", "--version"};
    auto r = proc::run_capture(argv);
    REQUIRE_FALSE(r.has_value());
    CHECK(r.error().find("grab-definitely-not-installed-xyz") != std::string::npos);
    CHECK_FALSE(proc::run_inherit(argv).has_value());
}

#ifdef _WIN32
TEST_CASE("the player's environment: no RCLONE_* variable at all, the rest kept (review G7, partly)") {
    SetEnvironmentVariableW(L"RCLONE_CONFIG_MAIN_PASS", L"secret");
    SetEnvironmentVariableW(L"rclone_progress", L"1");
    SetEnvironmentVariableW(L"GRAB_PLAYER_TEST_KEPT", L"yes");
    const std::wstring block = proc::environment_without_rclone();
    SetEnvironmentVariableW(L"RCLONE_CONFIG_MAIN_PASS", nullptr);
    SetEnvironmentVariableW(L"rclone_progress", nullptr);
    SetEnvironmentVariableW(L"GRAB_PLAYER_TEST_KEPT", nullptr);
    std::vector<std::wstring> entries;
    for (std::size_t at = 0; at < block.size() && block[at] != L'\0';) {
        const auto end = block.find(L'\0', at);
        entries.push_back(block.substr(at, end - at));
        at = end + 1;
    }
    REQUIRE(block.ends_with(std::wstring(2, L'\0')));
    CHECK(std::ranges::find(entries, L"GRAB_PLAYER_TEST_KEPT=yes") != entries.end());
    CHECK(std::ranges::none_of(entries, [](const std::wstring& e) {
        return util::to_lower(util::to_utf8(e.substr(0, 7))) == "rclone_";
    }));
    CHECK(std::ranges::any_of(entries, [](const std::wstring& e) { return e.starts_with(L"PATH=") || e.starts_with(L"Path="); }));
}
#endif
