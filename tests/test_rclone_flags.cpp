#include "process.hpp"
#include "rclone_flags.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (SetEnvironmentVariableW)
#endif

#include <string>
#include <vector>

using namespace grab;

TEST_CASE("grab's own rclone calls take only connection flags (G182, G184-G187)") {
    using V = std::vector<std::string>;
    // grab's defaults: -P goes, the SFTP ones stay.
    CHECK(rclone_flags::connection(V{"-P", "--sftp-chunk-size", "255Ki", "--sftp-disable-hashcheck"}) ==
          V{"--sftp-chunk-size", "255Ki", "--sftp-disable-hashcheck"});
    // Every spelling of progress and logging, filters, comparison and selection flags.
    CHECK(rclone_flags::connection(V{"-vP", "--progress=true", "-P=true", "--log-file", "C:\\logs\\r.log",
                                     "--log-level", "INFO", "--use-json-log", "-vv", "--exclude", "*.part",
                                     "--size-only", "--dry-run", "-n", "--transfers", "8", "--config", "x.conf"})
              .empty());
    // Connection flags with their values, either way of writing them.
    CHECK(rclone_flags::connection(V{"--bind=10.8.0.2", "--exclude", "*.part", "--sftp-ssh", "ssh -i key", "--timeout",
                                     "2m", "--contimeout=5s", "--sftp-key-exchange", "diffie-hellman-group14-sha1"}) ==
          V{"--bind=10.8.0.2", "--sftp-ssh", "ssh -i key", "--timeout", "2m", "--contimeout=5s", "--sftp-key-exchange",
            "diffie-hellman-group14-sha1"});
    CHECK(rclone_flags::is_connection("--sftp-concurrency=4"));
    CHECK(rclone_flags::is_connection("--password-command"));
    CHECK_FALSE(rclone_flags::is_connection("--bwlimit"));
    CHECK_FALSE(rclone_flags::is_connection("--binder"));
}

TEST_CASE("a flag after -- that grab's engine doesn't use, with what to use instead") {
    using V = std::vector<std::string>;
    CHECK_FALSE(rclone_flags::first_unusable(V{"--sftp-chunk-size", "1Mi", "--bind", "10.0.0.2"}).has_value());
    CHECK(rclone_flags::first_unusable(V{"--bind", "10.0.0.2", "--dry-run"})->flag == "--dry-run");
    CHECK(rclone_flags::first_unusable(V{"-n"})->hint.find("-n") != std::string::npos);
    CHECK(rclone_flags::first_unusable(V{"--bwlimit=5M"})->hint.find("--limit") != std::string::npos);
    CHECK(rclone_flags::first_unusable(V{"-P"})->hint.find("own progress") != std::string::npos);
    CHECK(rclone_flags::first_unusable(V{"--size-only"})->hint.find("--rclone") != std::string::npos);
}

TEST_CASE("which RCLONE_* variables grab's own rclone calls see (G8)") {
    CHECK(rclone_flags::kept_variable("PATH"));
    CHECK(rclone_flags::kept_variable("RCLONE_CONFIG"));
    CHECK(rclone_flags::kept_variable("RCLONE_CONFIG_PASS"));
    CHECK(rclone_flags::kept_variable("RCLONE_CONFIG_MY-BOX_PASS"));
    CHECK(rclone_flags::kept_variable("RCLONE_SFTP_CHUNK_SIZE"));
    CHECK(rclone_flags::kept_variable("RCLONE_PASSWORD_COMMAND"));
    CHECK(rclone_flags::kept_variable("rclone_config_box_pass")); // Windows ignores case
    CHECK_FALSE(rclone_flags::kept_variable("RCLONE_PROGRESS"));
    CHECK_FALSE(rclone_flags::kept_variable("rclone_progress"));
    CHECK_FALSE(rclone_flags::kept_variable("RCLONE_LOG_FILE"));
    CHECK_FALSE(rclone_flags::kept_variable("RCLONE_DRY_RUN"));
    CHECK_FALSE(rclone_flags::kept_variable("RCLONE_PASSWORD_COMMANDX"));
}

#ifdef _WIN32
TEST_CASE("children grab runs itself don't see RCLONE_PROGRESS and the like; run_inherit's do (G8)") {
    SetEnvironmentVariableW(L"RCLONE_PROGRESS", L"true");
    SetEnvironmentVariableW(L"RCLONE_SFTP_GRABTEST", L"kept");
    SetEnvironmentVariableW(L"RCLONE_CONFIG_GRABTEST_PASS", L"kept too");
    proc::RunOptions quiet;
    quiet.detached = true;
    const auto ran = proc::run_capture(std::vector<std::string>{"cmd", "/d", "/c", "set RCLONE_"}, quiet);
    SetEnvironmentVariableW(L"RCLONE_PROGRESS", nullptr);
    SetEnvironmentVariableW(L"RCLONE_SFTP_GRABTEST", nullptr);
    SetEnvironmentVariableW(L"RCLONE_CONFIG_GRABTEST_PASS", nullptr);
    REQUIRE(ran.has_value());
    CHECK(ran->out.find("RCLONE_PROGRESS") == std::string::npos);
    CHECK(ran->out.find("RCLONE_SFTP_GRABTEST=kept") != std::string::npos);
    CHECK(ran->out.find("RCLONE_CONFIG_GRABTEST_PASS=kept too") != std::string::npos);
    // The rest of the environment is there (cmd needs SystemRoot, for one).
    const auto path = proc::run_capture(std::vector<std::string>{"cmd", "/d", "/c", "echo %SystemRoot%"}, quiet);
    REQUIRE(path.has_value());
    CHECK(util::to_lower(path->out).find("windows") != std::string::npos);
}
#endif
