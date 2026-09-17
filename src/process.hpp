#pragma once

#include <expected>
#include <span>
#include <string>

namespace grab::proc {

struct CaptureResult {
    int exit_code = 0;
    std::string out; // child's stdout, raw bytes
};

// Run argv[0] with stdout captured; stdin and stderr stay attached to the console so an
// ssh passphrase prompt still works. Ctrl+C is left to the child while it runs.
[[nodiscard]] std::expected<CaptureResult, std::string> run_capture(std::span<const std::string> argv);

// Run argv[0] with all three streams inherited (live progress output). Returns its exit code.
[[nodiscard]] std::expected<int, std::string> run_inherit(std::span<const std::string> argv);

} // namespace grab::proc
