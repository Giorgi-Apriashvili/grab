#pragma once

// For the live cases that name a server by the settings the programs use (GRAB_TEST_SERVER and
// GRAB_TEST_APPDATA: test_live_server.cpp, test_live_upload.cpp): the server as engine::ssh_target
// loads it, read only.

#include "config.hpp"
#include "engine.hpp"
#include "ini.hpp"
#include "passwords.hpp"
#include "rclone_import.hpp"
#include "server_conf.hpp"
#include "ssh.hpp"
#include "ssh_target.hpp"
#include "util.hpp"

#include <algorithm>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)
#endif

namespace grab::test {

#ifdef _WIN32
// APPDATA pointed at `dir` while it lives, so grab's own files are the programs' ones.
class AppData {
public:
    explicit AppData(const std::filesystem::path& dir) : old_(util::getenv_utf8("APPDATA")) {
        SetEnvironmentVariableW(L"APPDATA", dir.c_str());
    }
    ~AppData() {
        SetEnvironmentVariableW(L"APPDATA", old_ ? util::path_from_utf8(*old_).c_str() : nullptr);
    }
    AppData(const AppData&) = delete;
    AppData& operator=(const AppData&) = delete;

private:
    std::optional<std::string> old_;
};
#endif

// The import's store for a server read through its pure function (read_context), else none.
inline std::optional<passwords::Store> planned_store;

// The server as the programs load it, read only: engine::load_context would also run the import
// from rclone.conf (writing grab.conf, the store and known_hosts) and needs rclone.exe. A section
// without connection details is read as the import would fill it in (rclone_import's pure
// function over the files as they are; nothing written). grab.conf: GRAB_TEST_CONFIG, else the
// one in GRAB_TEST_APPDATA (the runner clears GRAB_CONFIG, test_main.cpp).
inline std::expected<engine::Context, std::string> read_context(const std::string& name) {
    const auto conf = util::getenv_utf8("GRAB_TEST_CONFIG") ? util::path_from_utf8(*util::getenv_utf8("GRAB_TEST_CONFIG"))
                                                            : default_grab_config_path();
    auto doc = ini::parse_file(conf);
    if (!doc) return std::unexpected(doc.error());
    auto cfg = parse_grab_config(*doc);
    if (!cfg) return std::unexpected(cfg.error());
    auto selected = cfg->select(name);
    if (!selected) return std::unexpected(selected.error());
    const RemoteSettings settings = **selected;
    auto connection = server_conf::parse(*doc->find(settings.name));
    if (!connection) return std::unexpected(connection.error());
    planned_store.reset();
    if (!*connection) {
        auto in = rclone_import::read_inputs(rclone_import::files_for(conf));
        if (!in) return std::unexpected(in.error());
        auto plan = rclone_import::plan(*in);
        if (!plan) return std::unexpected(plan.error());
        const auto found = std::ranges::find(plan->imported, settings.name, &rclone_import::Imported::name);
        if (found == plan->imported.end()) return std::unexpected(std::string("the import wouldn't fill it in"));
        *connection = found->connection;
        planned_store = std::move(plan->store);
    }
    return engine::make_context(std::move(*cfg), settings, std::move(**connection), conf);
}

// grab's target for the server read_context read: engine::ssh_target's, or with the store the
// import's pure function planned for it.
inline std::expected<ssh::Target, std::string> live_target(const engine::Context& ctx) {
    if (!planned_store) return engine::ssh_target(ctx);
    auto target = ssh::target_for(ctx.settings.name, ctx.connection, *planned_store,
                                  util::path_from_utf8(ctx.remote.known_hosts_file.value_or("")));
    if (!target) return target;
    auto reach = engine::connect_settings(ctx.settings);
    if (!reach) return std::unexpected(reach.error());
    if (reach->bind) target->bind = *reach->bind;
    if (reach->connect_timeout) target->connect_timeout = *reach->connect_timeout;
    if (reach->io_timeout) target->timeout = *reach->io_timeout;
    return target;
}

} // namespace grab::test
