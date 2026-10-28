#pragma once

// Play in the window: the player. The stream it plays (a local HTTP server whose bytes come
// through grab's own connections to the server) is play::Server, src/play_stream.hpp.

#include <expected>
#include <string>
#include <string_view>

namespace grab::gui {

// Opens `url` in the program Windows opens files ending in `.ext` with ("mpc-hc64.exe" here),
// returning its name. Fails when that is no desktop program that takes an address (a Store app),
// or there is none. The player gets this process's environment without any RCLONE_* variable
// (review G7, partly). Debug builds: GRAB_TEST_PLAYER=<exe> stands in for it, for tests.
[[nodiscard]] std::expected<std::string, std::string> open_with_player(std::string_view ext, const std::string& url);

} // namespace grab::gui
