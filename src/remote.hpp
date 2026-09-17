#pragma once

#include "cli.hpp"
#include "config.hpp"

#include <expected>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace grab {

struct FindRequest {
    Mode mode = Mode::folder;
    std::string target;              // name pattern or absolute remote path
    std::vector<std::string> roots;  // ignored when target is absolute
    int max_depth = 4;
    bool skip_hidden = true;
};

[[nodiscard]] bool is_absolute_target(std::string_view target);

// The `find ... -print0` command run by the remote shell.
[[nodiscard]] std::string build_find_command(const FindRequest& req);

// ssh argv: options first, then user@host, then the remote command as one argument.
[[nodiscard]] std::vector<std::string> build_ssh_argv(const std::string& ssh_exe,
                                                      const RcloneRemote& remote,
                                                      std::span<const std::string> ssh_options,
                                                      const std::string& remote_command);

// Split NUL-separated find output into paths.
[[nodiscard]] std::vector<std::string> parse_find_output(std::string_view out);

// Shallowest first, then lexical.
void rank_matches(std::vector<std::string>& matches);

// Pick one match: the only one, the first when `first`, otherwise ask on `err`/`in`.
// `interactive` says whether asking is possible at all (stdin is a terminal).
[[nodiscard]] std::expected<std::string, std::string> choose_match(std::vector<std::string> matches,
                                                                   bool first, bool interactive,
                                                                   std::istream& in,
                                                                   std::ostream& err);

} // namespace grab
