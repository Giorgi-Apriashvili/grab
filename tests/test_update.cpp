#include "update.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
TEST_CASE("release programs: grab.exe first, bundled programs when built") {
    const auto programs = update::release_programs();
    REQUIRE_FALSE(programs.empty());
    CHECK(programs.front() == "grab.exe");
#ifdef _WIN32
    // The default Windows build bundles rclone and builds the GUI.
    CHECK(std::ranges::find(programs, "rclone.exe") != programs.end());
    CHECK(std::ranges::find(programs, "grab-gui.exe") != programs.end());
#endif
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
#endif
