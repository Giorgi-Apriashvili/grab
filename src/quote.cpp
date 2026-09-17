#include "quote.hpp"

#include <cctype>

namespace grab::quote {

std::string windows_arg(std::string_view arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string_view::npos) {
        return std::string(arg);
    }
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (const char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            // Backslashes before a quote must be doubled, then the quote itself escaped.
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
            backslashes = 0;
            continue;
        }
        out.append(backslashes, '\\');
        backslashes = 0;
        out += c;
    }
    // Trailing backslashes are followed by the closing quote, so they must be doubled.
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

std::string windows_cmdline(std::span<const std::string> argv) {
    std::string out;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) out += ' ';
        out += windows_arg(argv[i]);
    }
    return out;
}

std::string sh_single(std::string_view s) {
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    out += '\'';
    return out;
}

std::string sh_arg(std::string_view arg) {
    if (arg.empty()) return "''";
    constexpr std::string_view safe_extra = "_-./:=@%+,";
    for (const char c : arg) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) == 0 && safe_extra.find(c) == std::string_view::npos) {
            return sh_single(arg);
        }
    }
    return std::string(arg);
}

std::string display_cmdline(std::span<const std::string> argv) {
#ifdef _WIN32
    return windows_cmdline(argv);
#else
    std::string out;
    for (std::size_t i = 0; i < argv.size(); ++i) {
        if (i > 0) out += ' ';
        out += sh_arg(argv[i]);
    }
    return out;
#endif
}

} // namespace grab::quote
