#pragma once

// The window's previews at run time (see preview.hpp): one local `rclone serve http` per server
// (and per root, for paths outside the login home), started on first use and ended with
// grab-gui; files read from it for the viewer; videos and audio handed to the program Windows
// plays them with.

#include "engine.hpp"

#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace grab::gui {

class PreviewServers {
public:
    PreviewServers();
    ~PreviewServers(); // ends the servers
    PreviewServers(const PreviewServers&) = delete;
    PreviewServers& operator=(const PreviewServers&) = delete;

    // "http://127.0.0.1:PORT/TOKEN/" of `remote`'s server for `root` ("" the login home, "/"),
    // starting it first if it isn't running (about 1.5 s; up to 20 s on a slow login). Any thread.
    [[nodiscard]] std::expected<std::string, std::string> base(const engine::Context& ctx, const std::string& remote,
                                                               const std::string& root);
    // Whether that server runs already (base() then answers at once).
    [[nodiscard]] bool running(const std::string& remote, const std::string& root);

private:
    struct Server {
        HANDLE process = nullptr;
        std::string base;
        std::filesystem::path log;
    };
    std::mutex mutex_;
    std::map<std::pair<std::string, std::string>, Server> servers_;
    HANDLE job_ = nullptr; // kill-on-close: the servers end with grab-gui, even when it crashes
};

// A local server's answer to a GET of `url`: at most the first `max` bytes (a Range request);
// `total` the file's whole size, as the server says.
struct Fetched {
    int status = 0;
    std::string body;
    std::optional<std::uint64_t> total;
};
// `on_progress`: the bytes so far, and the most there will be, as they arrive.
[[nodiscard]] std::expected<Fetched, std::string>
http_get(const std::string& url, std::size_t max,
         const std::function<void(std::uint64_t done, std::uint64_t total)>& on_progress = {});

// Opens `url` in the program Windows opens files ending in `.ext` with ("mpc-hc64.exe" here),
// returning its name. Fails when that is no desktop program that takes an address (a Store app),
// or there is none. Debug builds: GRAB_TEST_PLAYER=<exe> stands in for it, for tests.
[[nodiscard]] std::expected<std::string, std::string> open_with_player(std::string_view ext, const std::string& url);

} // namespace grab::gui
