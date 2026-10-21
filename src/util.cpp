#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <iterator>
#include <system_error>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)

#include <bcrypt.h>
#include <dpapi.h>
#else
#include <fcntl.h>
#include <termios.h>
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

std::string format_size(std::uint64_t bytes) {
    if (bytes < 1024) return std::format("{} B", bytes);
    constexpr const char* units[] = {"KiB", "MiB", "GiB", "TiB", "PiB"};
    double value = static_cast<double>(bytes) / 1024.0;
    std::size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    return std::format("{:.1f} {}", value, units[unit]);
}

bool write_file_atomic(const std::filesystem::path& p, std::string_view text) {
    std::error_code ec;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    auto tmp = p;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out || !(out << text)) return false;
    }
    std::filesystem::rename(tmp, p, ec);
    return !ec;
}

bool write_new_file(const std::filesystem::path& p, std::string_view text) {
    bool ok = true;
#ifdef _WIN32
    const HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    while (ok && !text.empty()) {
        DWORD written = 0;
        const DWORD n = static_cast<DWORD>(std::min<std::size_t>(text.size(), 1u << 30));
        ok = WriteFile(h, text.data(), n, &written, nullptr) && written > 0;
        text.remove_prefix(written);
    }
    ok = CloseHandle(h) && ok;
#else
    const int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return false;
    while (ok && !text.empty()) {
        const ssize_t written = ::write(fd, text.data(), text.size());
        ok = written > 0;
        if (ok) text.remove_prefix(static_cast<std::size_t>(written));
    }
    ok = ::close(fd) == 0 && ok;
#endif
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(p, ec); // made just now, by us
    }
    return ok;
}

std::string escape_field(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '\t': out += "\\t"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        default: out += c;
        }
    }
    return out;
}

std::optional<std::string> unescape_field(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\') {
            out += s[i];
            continue;
        }
        if (++i == s.size()) return std::nullopt;
        switch (s[i]) {
        case '\\': out += '\\'; break;
        case 't': out += '\t'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        default: return std::nullopt;
        }
    }
    return out;
}

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::optional<std::int64_t> parse_unix_time(std::string_view text) {
    if (const auto dot = text.find('.'); dot != std::string_view::npos) {
        const auto fraction = text.substr(dot + 1);
        if (fraction.empty() || !std::ranges::all_of(fraction, [](char c) { return c >= '0' && c <= '9'; })) {
            return std::nullopt;
        }
        text = text.substr(0, dot);
    }
    std::int64_t n = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), n); // a '-', no '+'
    if (ec != std::errc{} || ptr != text.data() + text.size()) return std::nullopt;
    return n;
}

namespace {

struct Rfc3339 {
    int year = 0;
    std::int64_t seconds = 0; // since 1970 UTC
    std::int64_t nanos = 0;   // within the second
};

std::optional<Rfc3339> split_rfc3339(std::string_view t) {
    auto digits = [&](std::size_t pos, std::size_t n) -> std::optional<int> {
        if (pos + n > t.size()) return std::nullopt;
        int v = 0;
        for (std::size_t i = pos; i < pos + n; ++i) {
            if (t[i] < '0' || t[i] > '9') return std::nullopt;
            v = v * 10 + (t[i] - '0');
        }
        return v;
    };
    // YYYY-MM-DDTHH:MM:SS
    if (t.size() < 20 || t[4] != '-' || t[7] != '-' || (t[10] != 'T' && t[10] != 't' && t[10] != ' ') ||
        t[13] != ':' || t[16] != ':') {
        return std::nullopt;
    }
    auto y = digits(0, 4), mo = digits(5, 2), d = digits(8, 2), h = digits(11, 2), mi = digits(14, 2),
         s = digits(17, 2);
    if (!y || !mo || !d || !h || !mi || !s || *mo < 1 || *mo > 12 || *d < 1 || *d > 31) return std::nullopt;
    std::size_t pos = 19;
    std::int64_t nanos = 0;
    if (pos < t.size() && t[pos] == '.') {
        ++pos;
        int n = 0;
        while (pos < t.size() && t[pos] >= '0' && t[pos] <= '9') {
            if (n < 9) {
                nanos = nanos * 10 + (t[pos] - '0');
                ++n;
            }
            ++pos;
        }
        for (; n < 9; ++n) nanos *= 10;
    }
    std::int64_t offset_s = 0;
    if (pos < t.size() && (t[pos] == 'Z' || t[pos] == 'z')) {
        ++pos;
    } else if (pos < t.size() && (t[pos] == '+' || t[pos] == '-')) {
        auto oh = digits(pos + 1, 2), om = digits(pos + 4, 2);
        if (!oh || !om || pos + 3 >= t.size() || t[pos + 3] != ':') return std::nullopt;
        offset_s = (*oh * 3600 + *om * 60) * (t[pos] == '-' ? -1 : 1);
        pos += 6;
    } else {
        return std::nullopt;
    }
    if (pos != t.size()) return std::nullopt;
    // Days since 1970-01-01 (civil calendar, H. Hinnant's days_from_civil).
    const int yy = *y - (*mo <= 2 ? 1 : 0);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(yy - era * 400);
    const unsigned mp = static_cast<unsigned>((*mo + 9) % 12);
    const unsigned doy = (153 * mp + 2) / 5 + static_cast<unsigned>(*d) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const std::int64_t days = static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
    return Rfc3339{*y, days * 86400 + *h * 3600 + *mi * 60 + *s - offset_s, nanos};
}

} // namespace

std::optional<std::int64_t> parse_rfc3339(std::string_view text) {
    const auto t = split_rfc3339(text);
    // int64 nanoseconds reach from 1677-09-21 to 2262-04-11; whole years, with any offset, fit.
    if (!t || t->year < 1678 || t->year > 2261) return std::nullopt;
    return t->seconds * 1'000'000'000 + t->nanos;
}

std::optional<std::int64_t> parse_rfc3339_seconds(std::string_view text) {
    const auto t = split_rfc3339(text);
    if (!t) return std::nullopt;
    return t->seconds;
}

std::string local_date(std::int64_t unix_seconds) {
    const auto t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
#ifdef _WIN32
    if (localtime_s(&tm, &t) != 0) return {};
#else
    if (localtime_r(&t, &tm) == nullptr) return {};
#endif
    return std::format("{:04}-{:02}-{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
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

std::filesystem::path local_data_home() {
    if (auto l = getenv_utf8("LOCALAPPDATA"); l && !l->empty()) return path_from_utf8(*l);
    return config_home();
}

bool stdin_is_tty() {
    // _isatty() is also true for other character devices such as NUL, which would make grab
    // prompt when started with stdin redirected; only a real console has a console mode.
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    return in != nullptr && in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode) != 0;
}

std::optional<std::string> read_secret_line() {
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool console = in != nullptr && in != INVALID_HANDLE_VALUE && GetConsoleMode(in, &mode);
    if (console) SetConsoleMode(in, mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));
    std::string line;
    const bool ok = static_cast<bool>(std::getline(std::cin, line));
    if (console) {
        SetConsoleMode(in, mode);
        std::fputs("\n", stderr); // the Enter the user typed was not echoed
    }
    if (!ok) return std::nullopt;
    if (line.ends_with('\r')) line.pop_back();
    return line;
}

std::filesystem::path self_exe_path() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return std::filesystem::path(buf);
        }
        buf.resize(buf.size() * 2); // truncated: long path
    }
}

namespace {

// A file's digest in lower-case hex, with Windows' CNG `algorithm` (`size` bytes of digest).
std::expected<std::string, std::string> hash_file(const std::filesystem::path& p, LPCWSTR algorithm,
                                                  std::size_t size, std::string_view name) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return failf("cannot open '{}'", path_to_utf8(p));

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, algorithm, nullptr, 0) < 0) {
        return failf("{} is not available (BCryptOpenAlgorithmProvider failed)", name);
    }
    struct Guard {
        BCRYPT_ALG_HANDLE& alg;
        BCRYPT_HASH_HANDLE& hash;
        ~Guard() {
            if (hash != nullptr) BCryptDestroyHash(hash);
            if (alg != nullptr) BCryptCloseAlgorithmProvider(alg, 0);
        }
    } guard{alg, hash};
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        return fail("BCryptCreateHash failed");
    }

    std::vector<char> buf(1 << 16);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = static_cast<ULONG>(in.gcount());
        if (n > 0 && BCryptHashData(hash, reinterpret_cast<PUCHAR>(buf.data()), n, 0) < 0) {
            return fail("BCryptHashData failed");
        }
    }
    if (in.bad()) return failf("error reading '{}'", path_to_utf8(p));

    std::vector<unsigned char> digest(size);
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) {
        return fail("BCryptFinishHash failed");
    }
    std::string hex;
    hex.reserve(size * 2);
    for (const unsigned char b : digest) hex += std::format("{:02x}", b);
    return hex;
}

} // namespace

std::expected<std::string, std::string> sha256_file(const std::filesystem::path& p) {
    return hash_file(p, BCRYPT_SHA256_ALGORITHM, 32, "SHA-256");
}

std::expected<std::string, std::string> md5_file(const std::filesystem::path& p) {
    return hash_file(p, BCRYPT_MD5_ALGORITHM, 16, "MD5");
}

namespace {

DATA_BLOB blob_of(std::string_view s) {
    return DATA_BLOB{static_cast<DWORD>(s.size()), reinterpret_cast<BYTE*>(const_cast<char*>(s.data()))};
}

// Copies a blob DPAPI allocated, then wipes and frees it.
std::string take(DATA_BLOB& b) {
    std::string out(reinterpret_cast<const char*>(b.pbData), b.cbData);
    SecureZeroMemory(b.pbData, b.cbData);
    LocalFree(b.pbData);
    return out;
}

} // namespace

std::optional<std::string> protect_for_user(std::string_view data, std::string_view purpose) {
    DATA_BLOB in = blob_of(data);
    DATA_BLOB entropy = blob_of(purpose);
    DATA_BLOB out{};
    if (!CryptProtectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return std::nullopt;
    return take(out);
}

std::optional<std::string> unprotect_for_user(std::string_view data, std::string_view purpose) {
    DATA_BLOB in = blob_of(data);
    DATA_BLOB entropy = blob_of(purpose);
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return std::nullopt;
    }
    return take(out);
}

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

std::filesystem::path local_data_home() {
    if (auto x = getenv_utf8("XDG_CACHE_HOME"); x && !x->empty()) return path_from_utf8(*x);
    return path_from_utf8(getenv_utf8("HOME").value_or(".")) / ".cache";
}

bool stdin_is_tty() { return isatty(STDIN_FILENO) != 0; }

std::optional<std::string> read_secret_line() {
    termios old{};
    const bool console = tcgetattr(STDIN_FILENO, &old) == 0;
    if (console) {
        termios quiet = old;
        quiet.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
    }
    std::string line;
    const bool ok = static_cast<bool>(std::getline(std::cin, line));
    if (console) {
        tcsetattr(STDIN_FILENO, TCSANOW, &old);
        std::fputs("\n", stderr);
    }
    if (!ok) return std::nullopt;
    return line;
}

std::filesystem::path self_exe_path() {
    std::error_code ec;
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : p;
}

std::expected<std::string, std::string> sha256_file(const std::filesystem::path&) {
    return fail("SHA-256 hashing is only implemented on Windows");
}

std::expected<std::string, std::string> md5_file(const std::filesystem::path&) {
    return fail("MD5 hashing is only implemented on Windows");
}

std::optional<std::string> protect_for_user(std::string_view data, std::string_view) { return std::string(data); }
std::optional<std::string> unprotect_for_user(std::string_view data, std::string_view) { return std::string(data); }

#endif

} // namespace grab::util
