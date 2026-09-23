#pragma once

#include "cli.hpp"

namespace grab {

// `grab server list | add [NAME] | trust NAME | remove NAME`. Returns the process exit code.
[[nodiscard]] int run_server_command(const Options& opts);

} // namespace grab
