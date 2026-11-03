#pragma once

// An environment variable set for one scope and put back as it was afterwards, also when a
// REQUIRE throws out of the test case: a guard variable left set (RCLONE_SFTP_SOCKS_PROXY) would
// refuse every server in every later case.

#include "util.hpp"

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (SetEnvironmentVariableW)
#else
#include <stdlib.h> // IWYU pragma: keep (setenv, unsetenv)
#endif

#include <optional>
#include <string>
#include <utility>

namespace grab::test {

class ScopedEnv {
public:
    ScopedEnv(std::string name, const char* value) : name_(std::move(name)), was_(util::getenv_utf8(name_.c_str())) {
        set(value);
    }
    ~ScopedEnv() { set(was_ ? was_->c_str() : nullptr); }
    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;
    ScopedEnv(ScopedEnv&&) = delete;
    ScopedEnv& operator=(ScopedEnv&&) = delete;

private:
    void set(const char* value) const {
#ifdef _WIN32
        SetEnvironmentVariableW(util::to_wide(name_).c_str(), value != nullptr ? util::to_wide(value).c_str() : nullptr);
#else
        if (value != nullptr) setenv(name_.c_str(), value, 1);
        else unsetenv(name_.c_str());
#endif
    }

    std::string name_;
    std::optional<std::string> was_;
};

} // namespace grab::test
