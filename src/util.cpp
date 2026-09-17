#include "util.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iterator>

#ifdef _WIN32
#include <windows.h>

#include <io.h>
#else
#include <unistd.h>
#endif

namespace grab::util {

std::string_view trim(std::string_view s) {
    constexpr std::string_view ws = " \t\r\n\v\f";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string_view::npos) return {};
    const auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

std::vector<std::string> split(std::string_view s, char sep) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start <= s.size()) {
        auto end = s.find(sep, start);
        if (end == std::string_view::npos) end = s.size();
        const auto item = trim(s.substr(start, end - start));
        if (!item.empty()) out.emplace_back(item);
        start = end + 1;
    }
    return out;
}

std::vector<std::string> split_args(std::string_view s) {
    std::vector<std::string> out;
    std::string cur;
    bool in_arg = false;
    char quote = 0;
    for (const char c : s) {
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            } else {
                cur += c;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            in_arg = true;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            if (in_arg) {
                out.push_back(cur);
                cur.clear();
                in_arg = false;
            }
            continue;
        }
        cur += c;
        in_arg = true;
    }
    if (in_arg) out.push_back(cur);
    return out;
}

std::string join(const std::vector<std::string>& items, std::string_view sep) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += sep;
        out += items[i];
    }
    return out;
}

std::string path_to_utf8(const std::filesystem::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

std::filesystem::path path_from_utf8(std::string_view s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

std::expected<std::string, std::string> read_file(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return failf("cannot open '{}'", path_to_utf8(p));
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

#ifdef _WIN32

std::wstring to_wide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string to_utf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}

std::optional<std::string> getenv_utf8(const char* name) {
    const std::wstring wname = to_wide(name);
    const DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return std::nullopt;
    std::wstring buf(n, L'\0');
    const DWORD written = GetEnvironmentVariableW(wname.c_str(), buf.data(), n);
    buf.resize(written);
    return to_utf8(buf);
}

std::filesystem::path config_home() {
    if (auto a = getenv_utf8("APPDATA"); a && !a->empty()) return path_from_utf8(*a);
    if (auto u = getenv_utf8("USERPROFILE"); u && !u->empty())
        return path_from_utf8(*u) / "AppData" / "Roaming";
    return std::filesystem::current_path();
}

bool stdin_is_tty() { return _isatty(_fileno(stdin)) != 0; }

#else

std::optional<std::string> getenv_utf8(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr) return std::nullopt;
    return std::string(v);
}

std::filesystem::path config_home() {
    if (auto x = getenv_utf8("XDG_CONFIG_HOME"); x && !x->empty()) return path_from_utf8(*x);
    if (auto h = getenv_utf8("HOME"); h && !h->empty()) return path_from_utf8(*h) / ".config";
    return std::filesystem::current_path();
}

bool stdin_is_tty() { return isatty(STDIN_FILENO) != 0; }

#endif

} // namespace grab::util
