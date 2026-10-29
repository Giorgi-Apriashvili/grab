#pragma once

// For the live cases that name a server by a made-up rclone.conf (GRAB_TEST_RCLONE_CONF and
// GRAB_TEST_REMOTE): the server as the import would move it into grab.conf
// (rclone_import::plan, the pure function), read only. Nothing is written: no grab.conf, store
// or known_hosts of the tests' scratch folder is touched.

#include "passwords.hpp"
#include "rclone_import.hpp"
#include "util.hpp"

#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace grab::test {

struct LiveServer {
    rclone_import::Imported server;
    passwords::Store store; // its secrets, plain (an agent server has none)
};

inline std::expected<LiveServer, std::string> live_server(const std::filesystem::path& rclone_conf, std::string_view name,
                                                          const std::filesystem::path& known_hosts) {
    rclone_import::Inputs in;
    in.sources.push_back(rclone_import::read_source(rclone_conf));
    in.grab_text = "[" + std::string(name) + "]\n";
    in.known_hosts = known_hosts;
    in.known_hosts_text = util::read_file_if_exists(known_hosts).value_or(std::nullopt).value_or("");
    in.getenv = [](std::string_view v) { return util::getenv_utf8(std::string(v).c_str()); };
    in.exists = [](const std::filesystem::path& p) {
        std::error_code ec;
        return std::filesystem::is_regular_file(p, ec);
    };
    in.read_host_keys = [](const std::filesystem::path& p) {
        auto text = util::read_file(p);
        return text ? rclone_import::HostKeysFile{.text = std::move(*text), .error = {}}
                    : rclone_import::HostKeysFile{.text = std::nullopt, .error = text.error()};
    };
    auto plan = rclone_import::plan(in);
    if (!plan) return std::unexpected(plan.error());
    if (plan->imported.empty()) {
        std::string why = "not imported";
        for (const auto& note : plan->notes) why += "; " + note.text;
        return std::unexpected(why);
    }
    return LiveServer{.server = std::move(plan->imported.front()), .store = std::move(plan->store)};
}

} // namespace grab::test
