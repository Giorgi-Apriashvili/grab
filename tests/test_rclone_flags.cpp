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

TEST_CASE("which RCLONE_* variables grab's own rclone calls see (G8; phase 7)") {
    CHECK(rclone_flags::kept_variable("PATH"));
    CHECK(rclone_flags::kept_variable("RCLONE_SFTP_CHUNK_SIZE"));
    CHECK(rclone_flags::kept_variable("RCLONE_SFTP_KEY_EXCHANGE"));
    CHECK(rclone_flags::kept_variable("RCLONE_SFTP_HOSTS")); // not RCLONE_SFTP_HOST
    CHECK(rclone_flags::kept_variable("RCLONE_PASSWORD_COMMAND"));
    CHECK_FALSE(rclone_flags::kept_variable("RCLONE_PROGRESS"));
    CHECK_FALSE(rclone_flags::kept_variable("rclone_progress"));
    CHECK_FALSE(rclone_flags::kept_variable("RCLONE_LOG_FILE"));
    CHECK_FALSE(rclone_flags::kept_variable("RCLONE_DRY_RUN"));
    CHECK_FALSE(rclone_flags::kept_variable("RCLONE_PASSWORD_COMMANDX"));
    // Each run gets its own server from grab, its config in memory: no inherited rclone.conf path,
    // its password, or a server's variables (another grab's, the user's own).
    CHECK_FALSE(rclone_flags::kept_variable("RCLONE_CONFIG"));
    CHECK_FALSE(rclone_flags::kept_variable("RCLONE_CONFIG_PASS"));
    CHECK_FALSE(rclone_flags::kept_variable("RCLONE_CONFIG_MY-BOX_PASS"));
    CHECK_FALSE(rclone_flags::kept_variable("rclone_config_box_host")); // Windows ignores case
    // Nor what would beat the server's own address, login or host keys (each RCLONE_SFTP_* beats
    // RCLONE_CONFIG_<NAME>_*).
    for (const char* key : {"HOST", "USER", "PORT", "PASS", "KEY_PEM", "KEY_FILE", "KEY_FILE_PASS", "PUBKEY",
                            "PUBKEY_FILE", "KEY_USE_AGENT", "KNOWN_HOSTS_FILE", "ASK_PASSWORD", "SSH"}) {
        CHECK_FALSE_MESSAGE(rclone_flags::kept_variable(std::string("RCLONE_SFTP_") + key), key);
    }
    CHECK_FALSE(rclone_flags::kept_variable("rclone_sftp_known_hosts_file"));
}

TEST_CASE("--sftp-* flags that would replace the server grab gives rclone are refused (phase 7)") {
    using V = std::vector<std::string>;
    CHECK_FALSE(rclone_flags::first_refused(V{"-P", "--sftp-chunk-size", "255Ki", "--sftp-disable-hashcheck",
                                              "--sftp-key-exchange", "x", "--bind", "10.0.0.2"})
                    .has_value());
    const auto host =
        rclone_flags::first_refused(V{"--sftp-chunk-size", "1Mi", "--sftp-host=other.example", "--sftp-user", "x"})
            .value_or(rclone_flags::Refused{});
    CHECK(host.flag == "--sftp-host");
    CHECK(host.what == "address");
    for (const char* flag : {"--sftp-host", "--sftp-user", "--sftp-port", "--sftp-pass", "--sftp-key-pem",
                             "--sftp-key-file", "--sftp-key-file-pass", "--sftp-pubkey", "--sftp-pubkey-file",
                             "--sftp-key-use-agent", "--sftp-known-hosts-file", "--sftp-ask-password", "--sftp-ssh"}) {
        CHECK_MESSAGE(rclone_flags::first_refused(V{flag, "x"}).has_value(), flag);
    }
    CHECK(rclone_flags::first_refused(V{"--sftp-key-use-agent=false"}).value_or(rclone_flags::Refused{}).what ==
          "login method (ssh-agent)");
    // rclone's flag names are exact (no '_' for '-', no other case): these it refuses itself.
    CHECK_FALSE(rclone_flags::first_refused(V{"--sftp_host", "x", "--SFTP-HOST", "y"}).has_value());
}

#ifdef _WIN32
TEST_CASE("children grab runs itself don't see RCLONE_PROGRESS, RCLONE_CONFIG_* and the like; run_inherit's do (G8)") {
    SetEnvironmentVariableW(L"RCLONE_PROGRESS", L"true");
    SetEnvironmentVariableW(L"RCLONE_SFTP_GRABTEST", L"kept");
    SetEnvironmentVariableW(L"RCLONE_SFTP_HOST", L"elsewhere.example");
    SetEnvironmentVariableW(L"RCLONE_CONFIG_GRABTEST_PASS", L"not kept");
    proc::RunOptions quiet;
    quiet.detached = true;
    const auto ran = proc::run_capture(std::vector<std::string>{"cmd", "/d", "/c", "set RCLONE_"}, quiet);
    SetEnvironmentVariableW(L"RCLONE_PROGRESS", nullptr);
    SetEnvironmentVariableW(L"RCLONE_SFTP_GRABTEST", nullptr);
    SetEnvironmentVariableW(L"RCLONE_SFTP_HOST", nullptr);
    SetEnvironmentVariableW(L"RCLONE_CONFIG_GRABTEST_PASS", nullptr);
    REQUIRE(ran.has_value());
    CHECK(ran->out.find("RCLONE_PROGRESS") == std::string::npos);
    CHECK(ran->out.find("RCLONE_SFTP_GRABTEST=kept") != std::string::npos);
    CHECK(ran->out.find("RCLONE_SFTP_HOST") == std::string::npos);
    CHECK(ran->out.find("RCLONE_CONFIG_GRABTEST_PASS") == std::string::npos);
    // The rest of the environment is there (cmd needs SystemRoot, for one).
    const auto path = proc::run_capture(std::vector<std::string>{"cmd", "/d", "/c", "echo %SystemRoot%"}, quiet);
    REQUIRE(path.has_value());
    CHECK(util::to_lower(path->out).find("windows") != std::string::npos);
}
#endif
