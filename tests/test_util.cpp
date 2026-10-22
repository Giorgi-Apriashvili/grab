#include "file_list.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (SetEnvironmentVariableW)
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace grab;

#ifdef _WIN32
TEST_CASE("a profile folder with non-ASCII letters stays itself (G140)") {
    const auto saved = util::getenv_utf8("LOCALAPPDATA");
    const std::wstring local = L"C:\\Users\\\u10D2\u10D8\u10DD\u10E0\u10D2\u10D8\\AppData\\Local"; // გიორგი
    SetEnvironmentVariableW(L"LOCALAPPDATA", local.c_str());
    // Not C:\Users\áƒ’áƒ˜... (the UTF-8 bytes read as Windows-1252), where the window's browser
    // profile couldn't be created.
    CHECK(util::local_data_home() == std::filesystem::path(local));
    CHECK(file_list::default_dir() == std::filesystem::path(local) / L"grab" / L"lists");
    SetEnvironmentVariableW(L"LOCALAPPDATA", saved ? util::to_wide(*saved).c_str() : nullptr);
}
#endif

TEST_CASE("trim strips spaces, tabs and CR") {
    CHECK(util::trim("  abc \t\r\n") == "abc");
    CHECK(util::trim("") == "");
    CHECK(util::trim("   ") == "");
    CHECK(util::trim("x") == "x");
}

TEST_CASE("split trims items and drops empties") {
    const auto v = util::split(" /home/alice , /srv,, /opt ", ',');
    REQUIRE(v.size() == 3);
    CHECK(v[0] == "/home/alice");
    CHECK(v[1] == "/srv");
    CHECK(v[2] == "/opt");
    CHECK(util::split("", ',').empty());
    CHECK(util::split("   ", ',').empty());
}

TEST_CASE("split_args behaves like a shell for flag strings") {
    const auto v = util::split_args("-P  --sftp-chunk-size 255Ki   --exclude \"a b\" --x='c d'");
    REQUIRE(v.size() == 6);
    CHECK(v[0] == "-P");
    CHECK(v[1] == "--sftp-chunk-size");
    CHECK(v[2] == "255Ki");
    CHECK(v[3] == "--exclude");
    CHECK(v[4] == "a b");
    CHECK(v[5] == "--x=c d");
    CHECK(util::split_args("").empty());
    CHECK(util::split_args("   ").empty());
    CHECK(util::split_args("''").size() == 1);
}

TEST_CASE("join") {
    CHECK(util::join({}, ", ").empty());
    CHECK(util::join({"a"}, ", ") == "a");
    CHECK(util::join({"a", "b", "c"}, ", ") == "a, b, c");
}

TEST_CASE("Unix times as find and rclone print them") {
    CHECK(util::parse_unix_time("1697354553") == 1697354553);
    CHECK(util::parse_unix_time("1697354553.1234567000") == 1697354553); // find's %T@
    CHECK(util::parse_unix_time("0") == 0);
    CHECK(util::parse_unix_time("0.5") == 0);
    CHECK(util::parse_unix_time("-62135596800") == -62135596800); // rclone's unknown time
    CHECK(util::parse_unix_time("-1.5") == -1);
    constexpr const char* bad[] = {"",   "-",  ".5",  "5.",  "1.2.3", "+5",    " 5",
                                   "5 ", "12a", "1e9", "0x10", "7.5x", "99999999999999999999"};
    for (const char* text : bad) {
        CAPTURE(text);
        CHECK_FALSE(util::parse_unix_time(text).has_value());
    }
    // A listed time of 0 or before is unknown.
    CHECK(util::known_time(1) == 1);
    CHECK_FALSE(util::known_time(0).has_value());
    CHECK_FALSE(util::known_time(-62135596800).has_value());
    CHECK_FALSE(util::known_time(std::nullopt).has_value());
}

TEST_CASE("RFC 3339 times") {
    constexpr std::int64_t sec = 1'000'000'000;
    CHECK(util::parse_rfc3339("1970-01-01T00:00:00Z") == 0);
    CHECK(util::parse_rfc3339("2000-01-01T00:00:00Z") == std::int64_t{946684800} * sec);
    CHECK(util::parse_rfc3339("2000-01-01T01:00:00+01:00") == std::int64_t{946684800} * sec);
    CHECK(util::parse_rfc3339("2000-01-01T00:00:00.5Z") == std::int64_t{946684800} * sec + 500'000'000);
    CHECK_FALSE(util::parse_rfc3339("2000-01-01 garbage").has_value());
    CHECK_FALSE(util::parse_rfc3339("2000-01-01T00:00:00").has_value()); // no zone
    CHECK_FALSE(util::parse_rfc3339("2000-13-01T00:00:00Z").has_value());

    // 64 bits of nanoseconds reach from 1677 to 2262: the whole years between, any offset.
    CHECK(util::parse_rfc3339("1678-01-01T00:00:00Z") == std::int64_t{-9214560000} * sec);
    CHECK(util::parse_rfc3339("1678-01-01T00:00:00+23:59").has_value());
    CHECK(util::parse_rfc3339("2261-12-31T23:59:59.999999999-23:59").has_value());
    CHECK_FALSE(util::parse_rfc3339("1677-12-31T23:59:59Z").has_value());
    CHECK_FALSE(util::parse_rfc3339("2262-01-01T00:00:00Z").has_value());
    CHECK_FALSE(util::parse_rfc3339("0001-01-01T00:00:00Z").has_value());
    CHECK_FALSE(util::parse_rfc3339("9999-12-31T23:59:59Z").has_value());

    // In seconds, any year; the fraction is dropped.
    CHECK(util::parse_rfc3339_seconds("2023-10-15T11:22:33.987654321+02:00") == 1697361753);
    CHECK(util::parse_rfc3339_seconds("2023-10-15T09:22:33Z") == 1697361753);
    CHECK(util::parse_rfc3339_seconds("0001-01-01T00:00:00Z") == -62135596800);
    CHECK(util::parse_rfc3339_seconds("9999-12-31T23:59:59Z") == 253402300799);
    CHECK_FALSE(util::parse_rfc3339_seconds("yesterday").has_value());
    CHECK_FALSE(util::parse_rfc3339_seconds("").has_value());
}

TEST_CASE("a date in local time") {
    // 2023-10-15 05:00 UTC is the 14th or the 15th in every time zone (UTC-12 to UTC+14).
    const auto date = util::local_date(1697346000);
    CAPTURE(date);
    CHECK((date == "2023-10-14" || date == "2023-10-15"));
}

TEST_CASE("path round-trips through UTF-8") {
    const std::string s = "E:/Backup/Ünïcödé";
    CHECK(util::path_to_utf8(util::path_from_utf8(s)) == s);
}

TEST_CASE("data protected for the current user comes back only with the same purpose") {
    const std::string secret = "names of private files";
    const auto sealed = util::protect_for_user(secret, "grab test");
    REQUIRE(sealed.has_value());
    CHECK(util::unprotect_for_user(*sealed, "grab test") == secret);
#ifdef _WIN32
    CHECK(sealed->find("private") == std::string::npos);
    CHECK_FALSE(util::unprotect_for_user(*sealed, "something else").has_value());
    CHECK_FALSE(util::unprotect_for_user("not protected at all", "grab test").has_value());
    CHECK(util::unprotect_for_user(*util::protect_for_user("", "p"), "p") == std::string{});
#endif
}

TEST_CASE("a new file is written only where nothing is") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-new-file";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "sub");
    CHECK(util::write_new_file(dir / "a.txt", "one"));
    CHECK(util::read_file(dir / "a.txt").value_or("") == "one");
    CHECK_FALSE(util::write_new_file(dir / "a.txt", "two")); // there already: left alone
    CHECK(util::read_file(dir / "a.txt").value_or("") == "one");
    CHECK_FALSE(util::write_new_file(dir / "sub", "x")); // a folder
    CHECK(util::write_new_file(dir / "empty.txt", ""));
    CHECK(std::filesystem::file_size(dir / "empty.txt") == 0);
    std::filesystem::remove_all(dir);
}

namespace {

std::filesystem::path fresh_dir(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// The files in `dir` (names only), to see that nothing was left beside the target.
std::vector<std::string> names_in(const std::filesystem::path& dir) {
    std::vector<std::string> out;
    for (const auto& e : std::filesystem::directory_iterator(dir)) out.push_back(util::path_to_utf8(e.path().filename()));
    std::ranges::sort(out);
    return out;
}

} // namespace

TEST_CASE("two writers of one file never mix, and leave nothing beside it (G11)") {
    const auto dir = fresh_dir("grab-test-atomic");
    const auto file = dir / "grab.conf";
    const std::string a(3000, 'a');
    const std::string b(2000, 'b');
    int mixed = 0;
    int failed = 0;
    for (int round = 0; round < 100; ++round) {
        std::atomic<int> ready{0};
        std::atomic<bool> go{false};
        bool wrote_a = false;
        bool wrote_b = false;
        auto writer = [&](const std::string& text, bool& wrote) {
            ++ready;
            while (!go) {
            }
            wrote = util::write_file_atomic(file, text).has_value();
        };
        std::thread one(writer, std::cref(a), std::ref(wrote_a));
        std::thread two(writer, std::cref(b), std::ref(wrote_b));
        while (ready < 2) {
        }
        go = true;
        one.join();
        two.join();
        const auto got = util::read_file(file).value_or("");
        if (got != a && got != b) ++mixed;
        if (!wrote_a || !wrote_b) ++failed;
    }
    CHECK(mixed == 0);
    CHECK(failed == 0);
    CHECK(names_in(dir) == std::vector<std::string>{"grab.conf"});
    std::filesystem::remove_all(dir);
}

TEST_CASE("a failed write says why and leaves the file as it was") {
    const auto dir = fresh_dir("grab-test-atomic-fail");
    REQUIRE(util::write_file_atomic(dir / "blocker", "a file where a folder should be"));
    const auto written = util::write_file_atomic(dir / "blocker" / "grab.conf", "x");
    REQUIRE_FALSE(written.has_value());
    CHECK(written.error().starts_with("cannot write "));
    CHECK(written.error().size() > std::string("cannot write ").size() + util::path_to_utf8(dir).size() + 20);
    CHECK(names_in(dir) == std::vector<std::string>{"blocker"});
    std::filesystem::remove_all(dir);
}

TEST_CASE("missing, unreadable and readable files are told apart") {
    const auto dir = fresh_dir("grab-test-read");
    const auto missing = util::read_file_if_exists(dir / "none");
    REQUIRE(missing.has_value());
    CHECK_FALSE(missing->has_value());
    CHECK_FALSE(util::read_file(dir / "none").has_value());
    const auto no_folder = util::read_file_if_exists(dir / "no-folder" / "none");
    CHECK((no_folder.has_value() && !no_folder->has_value()));
    REQUIRE(util::write_file_atomic(dir / "empty", ""));
    const auto empty = util::read_file_if_exists(dir / "empty");
    CHECK((empty.has_value() && *empty == std::optional<std::string>("")));
    CHECK_FALSE(util::read_file_if_exists(dir).has_value()); // a folder: unreadable, not missing
    std::filesystem::remove_all(dir);
}

#ifdef _WIN32
TEST_CASE("a read that fails part-way is an error, not a shorter file (G216)") {
    const auto dir = fresh_dir("grab-test-read-tail");
    const auto file = dir / "known_hosts";
    REQUIRE(util::write_file_atomic(file, std::string(10000, 'k')));
    // Another program holds a lock on the tail.
    const HANDLE other = CreateFileW(file.c_str(), GENERIC_READ | GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(other != INVALID_HANDLE_VALUE);
    OVERLAPPED at{};
    at.Offset = 6000;
    REQUIRE(LockFileEx(other, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 100000, 0, &at));
    const auto read = util::read_file(file);
    CHECK_FALSE(read.has_value());
    if (!read) CHECK(read.error().starts_with("cannot read "));
    UnlockFileEx(other, 0, 100000, 0, &at);
    CloseHandle(other);
    CHECK(util::read_file(file).value_or("").size() == 10000);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a file being read can still be replaced, and another program's hold is waited out (G216)") {
    const auto dir = fresh_dir("grab-test-replace-open");
    const auto file = dir / "grab.conf";
    REQUIRE(util::write_file_atomic(file, "old"));
    // A reader that shares delete, as read_file opens files: the replace goes ahead at once.
    HANDLE reader = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(reader != INVALID_HANDLE_VALUE);
    CHECK(util::write_file_atomic(file, "new").has_value());
    CloseHandle(reader);
    CHECK(util::read_file(file).value_or("") == "new");

    // Another program that doesn't share delete, and lets go a moment later.
    reader = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(reader != INVALID_HANDLE_VALUE);
    std::thread let_go([reader] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CloseHandle(reader);
    });
    CHECK(util::write_file_atomic(file, "newer").has_value());
    let_go.join();
    CHECK(util::read_file(file).value_or("") == "newer");

    // One that never lets go: the write fails, the file stays as it was, nothing is left beside it.
    reader = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    REQUIRE(reader != INVALID_HANDLE_VALUE);
    CHECK_FALSE(util::write_file_atomic(file, "lost").has_value());
    CloseHandle(reader);
    CHECK(util::read_file(file).value_or("") == "newer");
    CHECK(names_in(dir) == std::vector<std::string>{"grab.conf"});
    std::filesystem::remove_all(dir);
}
#endif

TEST_CASE("a file moved aside keeps its text and never replaces another") {
    const auto dir = fresh_dir("grab-test-aside");
    const auto file = dir / "queue.json";
    REQUIRE(util::write_file_atomic(file, "one"));
    const auto first = util::move_aside(file, "bad");
    REQUIRE(first.has_value());
    CHECK(util::path_to_utf8(first->filename()).starts_with("queue.json.bad-"));
    CHECK_FALSE(std::filesystem::exists(file));
    REQUIRE(util::write_file_atomic(file, "two"));
    const auto second = util::move_aside(file, "bad"); // most likely the same second
    REQUIRE(second.has_value());
    CHECK(*second != *first);
    CHECK(util::read_file(*first).value_or("") == "one");
    CHECK(util::read_file(*second).value_or("") == "two");
    CHECK_FALSE(util::move_aside(file, "bad").has_value()); // gone already
    std::filesystem::remove_all(dir);
}

TEST_CASE("a file lock has one holder at a time") {
    const auto dir = fresh_dir("grab-test-lock");
    const auto at = dir / "settings.lock";
    auto held = util::lock_file(at, std::chrono::milliseconds(0));
    REQUIRE(held.has_value());
    bool second = true;
    std::thread other([&] { second = util::lock_file(at, std::chrono::milliseconds(100)).has_value(); });
    other.join();
    CHECK_FALSE(second);
    std::thread waiter([&] { second = util::lock_file(at, std::chrono::seconds(5)).has_value(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    *held = util::FileLock{}; // let go: the waiter gets it
    waiter.join();
    CHECK(second);
    std::filesystem::remove_all(dir);
}

TEST_CASE("a local path too long while Windows' long paths are off (G19)") {
    const auto dir = util::path_from_utf8("C:\\" + std::string(200, 'd'));
    const auto file = dir / std::string(50, 'f'); // 254 characters
    CHECK_FALSE(util::too_long(file, 0, false).has_value());
    CHECK_FALSE(util::too_long(file, 5, false).has_value()); // 259: still fits
    const std::string why = util::too_long(file, 16, false).value_or(""); // with ".2.grabpart.json"
    CHECK(why.find("270 characters") != std::string::npos);
    CHECK(why.find("long paths") != std::string::npos);
    CHECK_FALSE(util::too_long(file, 16, true).has_value());
    CHECK(util::long_paths() == util::long_paths()); // asked once
}

#ifdef _WIN32
TEST_CASE("a replaced file's temp name is short, whatever its own (G70)") {
    if (!util::long_paths()) return; // the path below is past 260 characters
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-long-name";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    // 248 characters, as a 234-character file's record: its name plus ".<8 hex>.tmp" would pass
    // Windows' 255 for one name.
    const auto record = dir / (std::string(234, 'n') + ".grabpart.json");
    REQUIRE(util::write_file_atomic(record, "one").has_value());
    REQUIRE(util::write_file_atomic(record, "two").has_value());
    CHECK(util::read_file(record).value_or("") == "two");
    CHECK(std::distance(std::filesystem::directory_iterator(dir), std::filesystem::directory_iterator()) == 1);
    std::filesystem::remove_all(dir);
}
#endif
