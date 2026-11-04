#include "update.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)

#include <sddl.h>
#endif

using namespace grab;
using update::Version;

TEST_CASE("versions parse with an optional leading v and compare numerically") {
    CHECK(update::parse_version("0.2.0") == Version{0, 2, 0});
    CHECK(update::parse_version("v1.10.3") == Version{1, 10, 3});
    CHECK(update::parse_version(" V2.0.0 ") == Version{2, 0, 0});
    CHECK_FALSE(update::parse_version("1.2").has_value());
    CHECK_FALSE(update::parse_version("1.2.3.4").has_value());
    CHECK_FALSE(update::parse_version("1.2.3-rc1").has_value());
    CHECK_FALSE(update::parse_version("v1.x.3").has_value());
    CHECK_FALSE(update::parse_version("").has_value());

    CHECK(Version{0, 2, 0} < Version{0, 10, 0}); // numeric, not lexical
    CHECK(Version{0, 9, 9} < Version{1, 0, 0});
    CHECK(Version{1, 2, 3} == Version{1, 2, 3});
    CHECK(update::to_string(Version{0, 2, 0}) == "0.2.0");
}

// Trimmed from a real api.github.com/repos/.../releases/latest response.
constexpr const char* release_json = R"({
  "url": "https://api.github.com/repos/Giorgi-Apriashvili/grab/releases/1",
  "tag_name": "v0.2.1",
  "name": "v0.2.1",
  "draft": false,
  "prerelease": false,
  "author": {"login": "Giorgi-Apriashvili", "id": 1},
  "assets": [
    {"name": "grab-0.2.1-windows-x64-setup.exe", "size": 2313099,
     "browser_download_url": "https://github.com/Giorgi-Apriashvili/grab/releases/download/v0.2.1/grab-0.2.1-windows-x64-setup.exe"},
    {"name": "grab-0.2.1-windows-x64.zip", "size": 280070,
     "browser_download_url": "https://github.com/Giorgi-Apriashvili/grab/releases/download/v0.2.1/grab-0.2.1-windows-x64.zip"},
    {"name": "SHA256SUMS.txt", "size": 190,
     "browser_download_url": "https://github.com/Giorgi-Apriashvili/grab/releases/download/v0.2.1/SHA256SUMS.txt"}
  ],
  "body": "## What's Changed\r\n* things \u2014 by @Giorgi-Apriashvili"
})";

TEST_CASE("release payload: tag, version and assets") {
    auto r = update::parse_release(release_json);
    REQUIRE_MESSAGE(r.has_value(), r.error_or(""));
    CHECK(r->tag == "v0.2.1");
    CHECK(r->version == Version{0, 2, 1});
    REQUIRE(r->assets.size() == 3);

    const auto zip = update::asset_name(r->version, update::InstallMode::portable);
    const auto setup = update::asset_name(r->version, update::InstallMode::installer);
    CHECK(zip == "grab-0.2.1-windows-x64.zip");
    CHECK(setup == "grab-0.2.1-windows-x64-setup.exe");
    REQUIRE(update::find_asset(*r, zip) != nullptr);
    CHECK(update::find_asset(*r, zip)->url.ends_with("/v0.2.1/grab-0.2.1-windows-x64.zip"));
    CHECK(update::find_asset(*r, setup) != nullptr);
    CHECK(update::find_asset(*r, "SHA256SUMS.txt") != nullptr);
    CHECK(update::find_asset(*r, "grab-9.9.9-windows-x64.zip") == nullptr);
}

TEST_CASE("release payload errors are readable") {
    auto not_found = update::parse_release(R"({"message": "Not Found", "status": "404"})");
    REQUIRE_FALSE(not_found.has_value());
    CHECK(not_found.error().find("Not Found") != std::string::npos);

    auto bad_tag = update::parse_release(R"({"tag_name": "nightly", "assets": []})");
    REQUIRE_FALSE(bad_tag.has_value());
    CHECK(bad_tag.error().find("nightly") != std::string::npos);

    CHECK_FALSE(update::parse_release("<html>rate limited</html>").has_value());
    CHECK_FALSE(update::parse_release("[]").has_value());

    auto no_assets = update::parse_release(R"({"tag_name": "v1.0.0"})");
    REQUIRE(no_assets.has_value());
    CHECK(no_assets->assets.empty());
}

TEST_CASE("SHA256SUMS lookup: two spaces or binary marker, CRLF, case folded") {
    const std::string a(64, 'a');
    const std::string b = "B" + std::string(63, 'b');
    const std::string sums = a + "  grab-0.2.1-windows-x64.zip\r\n" + b +
                             " *grab-0.2.1-windows-x64-setup.exe\r\n" + "garbage line\r\n";
    CHECK(update::find_sha256(sums, "grab-0.2.1-windows-x64.zip") == a);
    CHECK(update::find_sha256(sums, "grab-0.2.1-windows-x64-setup.exe") == std::string(64, 'b'));
    CHECK_FALSE(update::find_sha256(sums, "other.zip").has_value());
    CHECK_FALSE(update::find_sha256("abc  short.zip\n", "short.zip").has_value());
    CHECK_FALSE(update::find_sha256(std::string(64, 'z') + "  x.zip\n", "x.zip").has_value());
}

TEST_CASE("same_dir normalizes case, separators and trailing slashes") {
    // Inno stores InstallLocation with a trailing backslash; grab appends "\bin" to it.
    CHECK(update::same_dir(std::string("C:\\Users\\alice\\programs\\grab\\") + "\\bin",
                           "C:\\Users\\alice\\programs\\grab\\bin"));
    CHECK(update::same_dir("\\\\server\\share\\grab\\bin", "//SERVER/share/grab/bin/"));
    CHECK(update::same_dir("C:\\Users\\alice\\programs\\grab\\bin\\", "c:/users/ALICE/programs/grab/bin"));
    CHECK_FALSE(update::same_dir("C:\\a\\bin", "C:\\a\\bin2"));
}

#ifdef _WIN32
TEST_CASE("release programs: grab.exe first, the window when built, no rclone since 0.21") {
    const auto programs = update::release_programs();
    REQUIRE_FALSE(programs.empty());
    CHECK(programs.front() == "grab.exe");
    // The default Windows build builds the GUI.
    CHECK(std::ranges::find(programs, "grab-gui.exe") != programs.end());
    CHECK(std::ranges::find(programs, "rclone.exe") == programs.end());
}

TEST_CASE("missing programs in a portable folder") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-missing-programs";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "grab.exe") << "x";
    std::filesystem::create_directories(dir / "grab-gui.exe"); // a folder is not the program
    const std::vector<std::string> programs{"grab.exe", "rclone.exe", "grab-gui.exe"};
    CHECK(update::missing_programs(dir, programs) == std::vector<std::string>{"rclone.exe", "grab-gui.exe"});
    std::ofstream(dir / "rclone.exe") << "x";
    std::filesystem::remove(dir / "grab-gui.exe");
    std::ofstream(dir / "grab-gui.exe") << "x";
    CHECK(update::missing_programs(dir, programs).empty());
    std::filesystem::remove_all(dir);
}

TEST_CASE("only the programs an update replaced are deleted, not other programs' .old files (G31)") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-replaced-programs";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    for (const auto* name : {"grab.exe.old", "grab-gui.exe.old", "rclone.exe.grab-old", "rclone.exe.old",
                             "ffmpeg.exe.old", "grab.exe", "rclone.exe", "notes.old", "grab.exe.old.txt"}) {
        std::ofstream(dir / name) << "x";
    }
    std::filesystem::create_directories(dir / "tools.exe.old");
    update::remove_replaced_programs(dir);
    std::vector<std::string> left;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        left.push_back(util::path_to_utf8(e.path().filename()));
    }
    std::ranges::sort(left);
    // rclone.exe.old goes only when it is grab's old rclone (remove_old_rclone, below).
    CHECK(left == std::vector<std::string>{"ffmpeg.exe.old", "grab.exe", "grab.exe.old.txt", "notes.old", "rclone.exe",
                                           "rclone.exe.old", "tools.exe.old"});
    // Nothing to delete is no error, and a missing folder neither.
    update::remove_replaced_programs(dir);
    update::remove_replaced_programs(dir / "missing");
    std::filesystem::remove_all(dir);
}

namespace {

namespace fs = std::filesystem;
using update::OldRclone;

// A fresh folder in %TEMP%, gone again with the object.
struct Folder {
    fs::path dir;
    explicit Folder(const char* name) : dir(fs::temp_directory_path() / name) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);
    }
    ~Folder() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
    Folder(const Folder&) = delete;
    Folder& operator=(const Folder&) = delete;
};

void put(const fs::path& file, std::string_view bytes) { std::ofstream(file, std::ios::binary) << bytes; }

std::string bytes_of(const fs::path& file) { return util::read_file(file).value_or("(unreadable)"); }

std::vector<std::string> names_in(const fs::path& dir) {
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(dir)) names.push_back(util::path_to_utf8(e.path().filename()));
    std::ranges::sort(names);
    return names;
}

// A stand-in for grab's rclone 1.75.1: the tests give remove_old_rclone its size and SHA-256.
std::string grabs_rclone() { return "grab's old rclone, made up for the test " + std::string(200, 'r'); }

// A security descriptor from SDDL, set as the DACL of `p`.
bool set_dacl(const fs::path& p, const wchar_t* sddl) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return false;
    const bool ok = SetFileSecurityW(p.c_str(), DACL_SECURITY_INFORMATION, sd) != 0;
    LocalFree(sd);
    return ok;
}

} // namespace

TEST_CASE("grab's old rclone.exe goes by its exact size and SHA-256; any other rclone stays (G31)") {
    const Folder f("grab-test-old-rclone");
    const auto size = grabs_rclone().size();
    const auto sha256 = util::sha256_hex(grabs_rclone());

    put(f.dir / "rclone.exe", grabs_rclone());
    put(f.dir / "rclone.exe.old", grabs_rclone());
    put(f.dir / "grab.exe", "x");
    auto r = update::remove_old_rclone(f.dir, size, sha256);
    CHECK_FALSE(r.skipped);
    CHECK(r.exe == OldRclone::removed);
    CHECK(r.old == OldRclone::removed);
    CHECK(names_in(f.dir) == std::vector<std::string>{"grab.exe"});
    // Nothing left to do is no error.
    r = update::remove_old_rclone(f.dir, size, sha256);
    CHECK(r.exe == OldRclone::none);
    CHECK(r.old == OldRclone::none);

    // A decoy of the same size with other bytes is hashed and stays as it was.
    std::string decoy = grabs_rclone();
    decoy.back() = 'x';
    put(f.dir / "rclone.exe", decoy);
    // Another size is never opened: held without any sharing, an open would fail (not_opened).
    put(f.dir / "rclone.exe.old", grabs_rclone() + "!");
    HANDLE held = CreateFileW((f.dir / "rclone.exe.old").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);
    r = update::remove_old_rclone(f.dir, size, sha256);
    CloseHandle(held);
    CHECK(r.exe == OldRclone::other_file);
    CHECK(r.old == OldRclone::other_size);
    CHECK(bytes_of(f.dir / "rclone.exe") == decoy);
    CHECK(bytes_of(f.dir / "rclone.exe.old") == grabs_rclone() + "!");

    // A folder of that name isn't a file grab left.
    fs::remove(f.dir / "rclone.exe");
    fs::create_directories(f.dir / "rclone.exe");
    CHECK(update::remove_old_rclone(f.dir, size, sha256).exe == OldRclone::none);
    CHECK(fs::is_directory(f.dir / "rclone.exe"));
    // The real rule: 1.75.1's size, so neither of these is opened.
    CHECK(update::remove_old_rclone(f.dir).old == OldRclone::other_size);
    CHECK(update::old_rclone_size == 85'192'704);
}

TEST_CASE("grab's old rclone waits while a replaced grab.exe or grab-gui.exe is still there") {
    const Folder f("grab-test-old-rclone-skip");
    const auto size = grabs_rclone().size();
    const auto sha256 = util::sha256_hex(grabs_rclone());
    put(f.dir / "rclone.exe", grabs_rclone());
    for (const auto* replaced : {"grab-gui.exe.old", "grab.exe.old"}) {
        put(f.dir / replaced, "x"); // an old grab-gui may still run, and start rclone
        const auto r = update::remove_old_rclone(f.dir, size, sha256);
        CHECK(r.skipped);
        CHECK(r.exe == OldRclone::none);
        CHECK(bytes_of(f.dir / "rclone.exe") == grabs_rclone());
        fs::remove(f.dir / replaced);
    }
    CHECK(update::remove_old_rclone(f.dir, size, sha256).exe == OldRclone::removed);
    CHECK_FALSE(fs::exists(f.dir / "rclone.exe"));
}

TEST_CASE("grab's old rclone that runs is renamed aside, and deleted at a later start") {
    const Folder f("grab-test-old-rclone-held");
    // A real program, mapped as an image the way a running one is: it can be renamed, not deleted.
    std::wstring system(MAX_PATH, L'\0');
    system.resize(GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size())));
    REQUIRE_FALSE(system.empty());
    const auto file = f.dir / "rclone.exe";
    fs::copy_file(fs::path(system) / "version.dll", file);
    const auto size = fs::file_size(file);
    const auto sha256 = util::sha256_file(file);
    REQUIRE(sha256.has_value());

    HMODULE running = LoadLibraryExW(file.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    REQUIRE(running != nullptr);
    const auto r = update::remove_old_rclone(f.dir, size, *sha256);
    CHECK(r.exe == OldRclone::renamed);
    CHECK(names_in(f.dir) == std::vector<std::string>{"rclone.exe.grab-old"});
    update::remove_replaced_programs(f.dir); // still running: stays
    CHECK(names_in(f.dir) == std::vector<std::string>{"rclone.exe.grab-old"});
    FreeLibrary(running);
    update::remove_replaced_programs(f.dir); // the next start
    CHECK(names_in(f.dir).empty());
}

TEST_CASE("grab's old rclone in a folder grab can't change costs no hash (G102)") {
    const Folder f("grab-test-old-rclone-locked");
    const auto file = f.dir / "rclone.exe";
    put(file, grabs_rclone());
    // No delete access, neither on the file nor through its folder.
    REQUIRE(set_dacl(file, L"D:P(D;;SD;;;WD)(A;;FA;;;WD)"));
    REQUIRE(set_dacl(f.dir, L"D:P(D;;0x40;;;WD)(A;OICI;FA;;;WD)"));
    const auto r = update::remove_old_rclone(f.dir, grabs_rclone().size(), util::sha256_hex(grabs_rclone()));
    CHECK(set_dacl(f.dir, L"D:P(A;OICI;FA;;;WD)"));
    CHECK(set_dacl(file, L"D:P(A;;FA;;;WD)"));
    CHECK(r.exe == OldRclone::not_opened);
    CHECK(bytes_of(file) == grabs_rclone());
}

TEST_CASE("a link named rclone.exe is never followed to the rclone it leads to") {
    const Folder f("grab-test-old-rclone-link");
    fs::create_directories(f.dir / "bin");
    fs::create_directories(f.dir / "elsewhere");
    const auto target = f.dir / "elsewhere" / "rclone.exe";
    put(target, grabs_rclone());
    if (!CreateSymbolicLinkW((f.dir / "bin" / "rclone.exe").c_str(), target.c_str(),
                             SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
        MESSAGE("no symbolic links here (no Developer Mode or privilege); skipped");
        return;
    }
    const auto r = update::remove_old_rclone(f.dir / "bin", grabs_rclone().size(), util::sha256_hex(grabs_rclone()));
    CHECK(r.exe == OldRclone::none);
    CHECK(bytes_of(target) == grabs_rclone());
    CHECK(fs::is_symlink(f.dir / "bin" / "rclone.exe"));
}

TEST_CASE("grab's old rclone goes once per folder; one put there later is never touched") {
    const Folder f("grab-test-old-rclone-once");
    const Folder marks("grab-test-old-rclone-marks");
    const auto size = grabs_rclone().size();
    const auto sha256 = util::sha256_hex(grabs_rclone());
    const auto bin = f.dir / "bin";
    fs::create_directories(bin);

    // The first start after the update removes it and marks the folder done.
    put(bin / "rclone.exe", grabs_rclone());
    auto r = update::remove_old_rclone_once(bin, marks.dir, size, sha256);
    CHECK_FALSE(r.done_before);
    CHECK(r.exe == OldRclone::removed);
    CHECK(names_in(marks.dir) == std::vector<std::string>{update::folder_key(util::path_to_utf8(bin))});
    // The same build put there again later is the user's: nothing in the folder is looked at.
    put(bin / "rclone.exe", grabs_rclone());
    r = update::remove_old_rclone_once(bin, marks.dir, size, sha256);
    CHECK(r.done_before);
    CHECK(r.exe == OldRclone::none);
    CHECK(bytes_of(bin / "rclone.exe") == grabs_rclone());
    // The mark is the folder's, whatever its spelling.
    CHECK(update::remove_old_rclone_once(f.dir / "BIN", marks.dir, size, sha256).done_before);

    // Another folder has its own pass; one with nothing to remove is marked at once.
    const auto other = f.dir / "portable";
    fs::create_directories(other);
    put(other / "rclone.exe", "someone else's rclone");
    r = update::remove_old_rclone_once(other, marks.dir, size, sha256);
    CHECK_FALSE(r.done_before);
    CHECK(r.exe == OldRclone::other_size);
    CHECK(names_in(marks.dir).size() == 2);
    CHECK(bytes_of(other / "rclone.exe") == "someone else's rclone");
}

TEST_CASE("grab's old rclone isn't marked done while a pass leaves something to come back for") {
    const Folder f("grab-test-old-rclone-pending");
    const Folder marks("grab-test-old-rclone-pending-marks");
    const auto size = grabs_rclone().size();
    const auto sha256 = util::sha256_hex(grabs_rclone());

    // Skipped beside a replaced program (an old grab-gui may still run): the next start tries.
    put(f.dir / "rclone.exe", grabs_rclone());
    put(f.dir / "grab-gui.exe.old", "x");
    auto r = update::remove_old_rclone_once(f.dir, marks.dir, size, sha256);
    CHECK(r.skipped);
    CHECK_FALSE(fs::exists(marks.dir / update::folder_key(util::path_to_utf8(f.dir))));
    CHECK(bytes_of(f.dir / "rclone.exe") == grabs_rclone());
    fs::remove(f.dir / "grab-gui.exe.old");

    // In a folder grab can't change it stays, and so does the chance to remove it later.
    REQUIRE(set_dacl(f.dir / "rclone.exe", L"D:P(D;;SD;;;WD)(A;;FA;;;WD)"));
    REQUIRE(set_dacl(f.dir, L"D:P(D;;0x40;;;WD)(A;OICI;FA;;;WD)"));
    r = update::remove_old_rclone_once(f.dir, marks.dir, size, sha256);
    CHECK(set_dacl(f.dir, L"D:P(A;OICI;FA;;;WD)"));
    CHECK(set_dacl(f.dir / "rclone.exe", L"D:P(A;;FA;;;WD)"));
    CHECK(r.exe == OldRclone::not_opened);
    CHECK_FALSE(fs::exists(marks.dir / update::folder_key(util::path_to_utf8(f.dir))));

    // Running, it is renamed aside: not done until that rclone.exe.grab-old is gone too.
    fs::remove(f.dir / "rclone.exe");
    std::wstring system(MAX_PATH, L'\0');
    system.resize(GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size())));
    REQUIRE_FALSE(system.empty());
    fs::copy_file(fs::path(system) / "version.dll", f.dir / "rclone.exe");
    const auto real_size = fs::file_size(f.dir / "rclone.exe");
    const auto real_sha256 = util::sha256_file(f.dir / "rclone.exe");
    REQUIRE(real_sha256.has_value());
    HMODULE running = LoadLibraryExW((f.dir / "rclone.exe").c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    REQUIRE(running != nullptr);
    r = update::remove_old_rclone_once(f.dir, marks.dir, real_size, *real_sha256);
    CHECK(r.exe == OldRclone::renamed);
    update::remove_replaced_programs(f.dir); // a start while it still runs
    r = update::remove_old_rclone_once(f.dir, marks.dir, real_size, *real_sha256);
    CHECK_FALSE(r.done_before);
    CHECK_FALSE(fs::exists(marks.dir / update::folder_key(util::path_to_utf8(f.dir))));
    FreeLibrary(running);
    update::remove_replaced_programs(f.dir); // the start after it ended
    r = update::remove_old_rclone_once(f.dir, marks.dir, real_size, *real_sha256);
    CHECK(r.exe == OldRclone::none);
    CHECK(fs::exists(marks.dir / update::folder_key(util::path_to_utf8(f.dir))));
    CHECK(names_in(f.dir).empty());
}

TEST_CASE("grab's old rclone that can't be read now isn't taken for someone else's") {
    const Folder f("grab-test-old-rclone-unread");
    const Folder marks("grab-test-old-rclone-unread-marks");
    const auto size = grabs_rclone().size();
    const auto sha256 = util::sha256_hex(grabs_rclone());
    put(f.dir / "rclone.exe", grabs_rclone());

    // Held by another handle that lets it be opened for the delete, with every byte locked: the
    // hash's read fails (as on an I/O error or a placeholder offline).
    HANDLE held = CreateFileW((f.dir / "rclone.exe").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    REQUIRE(held != INVALID_HANDLE_VALUE);
    OVERLAPPED whole{};
    REQUIRE(LockFileEx(held, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &whole));
    auto r = update::remove_old_rclone_once(f.dir, marks.dir, size, sha256);
    CHECK(r.exe == OldRclone::unread);
    CHECK_FALSE(fs::exists(marks.dir / update::folder_key(util::path_to_utf8(f.dir))));
    CHECK(fs::exists(f.dir / "rclone.exe"));
    UnlockFileEx(held, 0, MAXDWORD, MAXDWORD, &whole);
    CloseHandle(held);

    // Readable again at the next start: removed, and the folder is done.
    CHECK(bytes_of(f.dir / "rclone.exe") == grabs_rclone());
    r = update::remove_old_rclone_once(f.dir, marks.dir, size, sha256);
    CHECK(r.exe == OldRclone::removed);
    CHECK(fs::exists(marks.dir / update::folder_key(util::path_to_utf8(f.dir))));
    CHECK_FALSE(fs::exists(f.dir / "rclone.exe"));
}

TEST_CASE("a folder's key is the installer's: SHA-256 of its lower-case path in UTF-16LE") {
    const auto key = update::folder_key(R"(C:\Users\Alice\programs\grab\bin)");
    // What Inno's GetSHA256OfUnicodeString(Lowercase('{app}\bin')) hashes for that folder.
    std::string utf16;
    for (const char c : std::string_view(R"(c:\users\alice\programs\grab\bin)")) {
        utf16 += c;
        utf16 += '\0';
    }
    CHECK(key == util::sha256_hex(utf16));
    CHECK(key.size() == 64);
    // Every spelling same_dir takes as that folder.
    CHECK(update::folder_key("c:/users/alice/programs/grab/bin/") == key);
    CHECK(update::folder_key("C:\\Users\\Alice\\programs\\grab\\\\bin\\") == key);
    CHECK(update::folder_key("C:\\Users\\Alice\\programs\\grab\\bin2") != key);
    // Beyond ASCII: UTF-16 code units, as Inno's strings are; only ASCII letters are lowered.
    const auto georgian = update::folder_key("C:\\Users\\გიორგი\\bin");
    std::string expected;
    for (const char16_t ch : std::u16string(u"c:\\users\\გიორგი\\bin")) {
        expected += static_cast<char>(ch & 0xFF);
        expected += static_cast<char>(ch >> 8);
    }
    CHECK(georgian == util::sha256_hex(expected));
}

TEST_CASE("one grab update at a time for a folder (G96)") {
    const auto dir = fs::temp_directory_path() / "grab-test-update-lock" / "bin";
    auto first = update::lock_folder(dir);
    REQUIRE(first.has_value());
    // The same folder, however it is written, is taken; another folder isn't.
    CHECK_FALSE(update::lock_folder(dir).has_value());
    CHECK_FALSE(update::lock_folder(util::path_from_utf8(util::to_lower(util::path_to_utf8(dir)) + "\\")).has_value());
    CHECK(update::lock_folder(dir.parent_path() / "other").has_value());
    first.reset();
    CHECK(update::lock_folder(dir).has_value());
}

TEST_CASE("what the last update did is kept, and a failure shows until something newer runs") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-last-update";
    std::filesystem::remove_all(dir);
    const auto file = dir / "last-update.json";
    const auto programs = dir / "bin"; // the copy whose update this is
    update::Result r;
    r.folder = update::folder_key(util::path_to_utf8(programs));
    r.from = "0.21.0";
    r.to = "0.21.1";
    r.mode = "installer";
    r.ok = false;
    r.exit_code = update::exit_update_failed;
    r.installer_exit = 5;
    r.error = "the installer failed (exit 5); the previous version was kept (setup log: C:\\T\\grab-setup-0.21.1.log)";
    r.setup_log = "C:\\T\\grab-setup-0.21.1.log";
    r.time = 1'762'300'000;
    const auto parsed = update::parse_result(update::result_json(r));
    REQUIRE(parsed.has_value());
    const auto back = parsed.value_or(update::Result{});
    CHECK(back.folder == r.folder);
    CHECK(back.from == r.from);
    CHECK(back.to == r.to);
    CHECK(back.mode == r.mode);
    CHECK_FALSE(back.ok);
    CHECK(back.exit_code == update::exit_update_failed);
    CHECK(back.installer_exit == std::optional<int>(5));
    CHECK(back.error == r.error);
    CHECK(back.setup_log == r.setup_log);
    CHECK(back.time == r.time);

    // Kept with what it printed beside it.
    REQUIRE(update::save_result(file, r, "grab 0.21.0: checking for updates...\ngrab: error: x\n").has_value());
    CHECK(util::read_file(dir / "last-update.log").value_or("") == "grab 0.21.0: checking for updates...\ngrab: error: x\n");
    const auto pending = update::pending_failure(file, Version{0, 21, 0}, programs);
    REQUIRE(pending.has_value());
    const auto failed = pending.value_or(update::Result{});
    CHECK(failed.to == "0.21.1");
    CHECK(update::failure_text(failed).starts_with("the update to 0.21.1 ("));
    CHECK(update::failure_text(failed).ends_with("failed: " + r.error));
    // Once that version (or a newer one) runs, it no longer matters.
    CHECK_FALSE(update::pending_failure(file, Version{0, 21, 1}, programs).has_value());
    CHECK_FALSE(update::pending_failure(file, Version{0, 22, 0}, programs).has_value());
    // Another copy's failure (a portable one sharing %LOCALAPPDATA%) isn't this one's, under any
    // spelling of this one's folder it is.
    CHECK(update::pending_failure(file, Version{0, 21, 0}, util::path_from_utf8(util::path_to_utf8(programs) + "\\"))
              .has_value());
    CHECK_FALSE(update::pending_failure(file, Version{0, 21, 0}, dir / "portable").has_value());
    // A record with no folder (none is older than 0.21, but a file edited by hand) is nobody's.
    update::Result nobodys = r;
    nobodys.folder.clear();
    REQUIRE(update::save_result(file, nobodys, "").has_value());
    CHECK_FALSE(update::pending_failure(file, Version{0, 21, 0}, programs).has_value());
    // A success, nothing kept, or something else in the file: nothing to show.
    r.ok = true;
    REQUIRE(update::save_result(file, r, "").has_value());
    CHECK_FALSE(update::pending_failure(file, Version{0, 21, 0}, programs).has_value());
    std::filesystem::remove(file);
    CHECK_FALSE(update::pending_failure(file, Version{0, 21, 0}, programs).has_value());
    std::ofstream(file, std::ios::binary) << R"({"to": "0.21.1")";
    CHECK_FALSE(update::pending_failure(file, Version{0, 21, 0}, programs).has_value());
    CHECK_FALSE(update::parse_result(R"({"to": "0.21.1"})").has_value()); // no outcome
    CHECK_FALSE(update::parse_result("[]").has_value());
    // Numbers out of range (a file edited by hand) count as missing.
    const auto odd = update::parse_result(R"({"ok": false, "to": "0.21.1", "exit": 1e20, "installerExit": -1e300,
                                              "time": 1e30})");
    REQUIRE(odd.has_value());
    CHECK(odd.value_or(update::Result{}).to == "0.21.1");
    CHECK(odd.value_or(update::Result{}).exit_code == 0);
    CHECK_FALSE(odd.value_or(update::Result{}).installer_exit.has_value());
    CHECK(odd.value_or(update::Result{}).time == 0);
    CHECK(update::parse_result(R"({"ok": false, "to": "0.21.1", "time": -5})").value_or(update::Result{}).time == 0);
    // Without an error line, the exit code says what little is known.
    update::Result bare;
    bare.to = "0.21.1";
    bare.exit_code = 5;
    CHECK(update::failure_text(bare) == "the update to 0.21.1 failed: grab update exited with code 5");
    std::filesystem::remove_all(dir);
}

TEST_CASE("install when newer, or the same version into an incomplete folder, never older") {
    const Version v020{0, 2, 0};
    const Version v030{0, 3, 0};
    CHECK(update::needs_install(v020, v030, false));
    CHECK(update::needs_install(v020, v030, true));
    CHECK_FALSE(update::needs_install(v030, v030, false));
    CHECK(update::needs_install(v030, v030, true)); // complete this release's folder
    CHECK_FALSE(update::needs_install(v030, v020, true)); // no downgrade, even if incomplete
}

TEST_CASE("sha256_file and self_exe_path") {
    const auto path = std::filesystem::temp_directory_path() / "grab_test_sha256.txt";
    {
        std::ofstream out(path, std::ios::binary);
        out << "abc";
    }
    auto hash = util::sha256_file(path);
    std::filesystem::remove(path);
    REQUIRE_MESSAGE(hash.has_value(), hash.error_or(""));
    CHECK(*hash == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK_FALSE(util::sha256_file(path).has_value()); // gone now

    const auto self = util::self_exe_path();
    CHECK(self.is_absolute());
    CHECK(self.filename() == "grab_tests.exe");
}

TEST_CASE("an up-to-date grab update forgets this copy's failure, of a release since withdrawn") {
    // The tests' own %LOCALAPPDATA% (a temp folder), never the user's.
    const auto record = update::result_path();
    REQUIRE(util::path_to_utf8(record).starts_with(util::path_to_utf8(fs::temp_directory_path())));
    const Folder f("grab-test-update-withdrawn");
    put(f.dir / "latest.json", R"({"tag_name": "v0.0.1", "assets": []})");
    std::string url = "file:///" + util::path_to_utf8(f.dir / "latest.json");
    std::ranges::replace(url, '\\', '/');
    SetEnvironmentVariableW(L"GRAB_UPDATE_API", util::to_wide(url).c_str());
    struct Unset {
        ~Unset() { SetEnvironmentVariableW(L"GRAB_UPDATE_API", nullptr); }
    } unset;

    update::Result failed;
    failed.folder = update::folder_key(util::path_to_utf8(util::self_exe_path().parent_path()));
    failed.to = "99.0.0"; // withdrawn: the latest release is older than this build now
    failed.error = "the installer failed (exit 5)";
    REQUIRE(update::save_result(record, failed, "").has_value());
    // --check changes nothing.
    CHECK(update::run(true) == 0);
    CHECK(fs::exists(record));
    // Another copy's record stays for that copy.
    update::Result others = failed;
    others.folder = update::folder_key("C:\\somewhere\\else");
    REQUIRE(update::save_result(record, others, "").has_value());
    CHECK(update::run(false) == 0);
    CHECK(fs::exists(record));
    // This copy's goes.
    REQUIRE(update::save_result(record, failed, "").has_value());
    CHECK(update::run(false) == 0);
    CHECK_FALSE(fs::exists(record));
    std::error_code ec;
    fs::remove(record.parent_path() / "last-update.log", ec);
}
#endif

TEST_CASE("the release page is kept only when it is a github.com page") {
    auto r = update::parse_release(
        R"({"tag_name":"v0.7.0","html_url":"https://github.com/Giorgi-Apriashvili/grab/releases/tag/v0.7.0","assets":[]})");
    REQUIRE(r.has_value());
    CHECK(r->page == "https://github.com/Giorgi-Apriashvili/grab/releases/tag/v0.7.0");
    auto other = update::parse_release(R"({"tag_name":"v0.7.0","html_url":"https://evil.example/x","assets":[]})");
    REQUIRE(other.has_value());
    CHECK(other->page.empty());
    CHECK_FALSE(update::is_release_page("https://github.com/"));
    CHECK_FALSE(update::is_release_page("http://github.com/x"));
    CHECK_FALSE(update::is_release_page("https://github.com.evil.example/x"));
    CHECK_FALSE(update::is_release_page("https://github.com/x\" onclick=\"y"));
    CHECK_FALSE(update::is_release_page("https://github.com/x y"));
    CHECK_FALSE(update::is_release_page("https://github.com/x\n"));
    CHECK(update::is_release_page("https://github.com/a/b/releases/tag/v1.0.0"));
}

TEST_CASE("each line grab update prints maps to its step") {
    using update::Phase;
    namespace t = update::text;
    CHECK(update::phase_of(std::string("grab 0.6.0: ") + std::string(t::checking)) == Phase::checking);
    CHECK(update::phase_of(std::string(t::downloading) + "grab-0.7.0-windows-x64-setup.exe (installer install)") ==
          Phase::downloading);
    CHECK(update::phase_of(std::string(t::verified) + "9131229c") == Phase::verified);
    CHECK(update::phase_of(t::installing) == Phase::installing);
    CHECK(update::phase_of(std::string(t::updated) + "0.6.0 -> 0.7.0") == Phase::done);
    CHECK(update::phase_of(std::string(t::completed) + "0.6.0 in C:\\grab (rclone.exe)") == Phase::done);
    CHECK(update::phase_of(std::string("grab 0.6.0") + std::string(t::up_to_date) + " (latest release: v0.6.0)") ==
          Phase::up_to_date);
    CHECK(update::phase_of(std::string(t::error) + "checksum mismatch") == Phase::failed);
    CHECK(update::phase_of("grab-gui is open: restart it to use the new version") == Phase::other);
    CHECK(update::phase_of("  \r") == Phase::other);
}

TEST_CASE("an update check is due once a day") {
    constexpr std::int64_t day = 24 * 60 * 60;
    const std::int64_t now = 1'760'000'000;
    CHECK(update::update_due(0, now)); // never checked
    CHECK(update::update_due(now - day, now));
    CHECK_FALSE(update::update_due(now - day + 60, now));
    CHECK_FALSE(update::update_due(now, now));
    CHECK(update::update_due(now + 3600, now)); // the clock went back
}
