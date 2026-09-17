#pragma once

#include <span>
#include <string>
#include <string_view>

namespace grab::quote {

// Quote one argument so the MSVC C runtime (and CommandLineToArgvW) parses it back verbatim.
[[nodiscard]] std::string windows_arg(std::string_view arg);

// Join argv into a single Windows command line for CreateProcess.
[[nodiscard]] std::string windows_cmdline(std::span<const std::string> argv);

// Single-quote for a POSIX shell: it's -> 'it'\''s'
[[nodiscard]] std::string sh_single(std::string_view s);

// Quote only when needed, for a POSIX shell.
[[nodiscard]] std::string sh_arg(std::string_view arg);

// Join argv into a copy-pastable line for the current platform's shell.
[[nodiscard]] std::string display_cmdline(std::span<const std::string> argv);

} // namespace grab::quote
