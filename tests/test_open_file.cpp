#include "open_file.hpp"
#include "scoped_env.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

using namespace grab;
namespace fs = std::filesystem;
using open_file::Facts;
using open_file::Found;

namespace {

// A type with nothing known of it but its "open" command.
bool runs(std::string_view command, std::string_view ext = ".xyz") {
    return open_file::runs_program(ext, Facts{.pathext = "", .open_command = std::string(command), .dangerous = false});
}

} // namespace

TEST_CASE("a file's extension as Windows takes it") {
    CHECK(open_file::extension_of("x.BAT") == ".bat");
    CHECK(open_file::extension_of(fs::path("D:/dl/f") / "run.bat. ") == ".bat"); // Windows drops the trailing dot and space
    CHECK(open_file::extension_of(".bat") == ".bat"); // all extension to Windows, none to std::filesystem
    CHECK(open_file::extension_of("archive.tar.gz") == ".gz");
    CHECK(open_file::extension_of("e01 (2).mkv") == ".mkv");
    CHECK(open_file::extension_of("README").empty());
    CHECK(open_file::extension_of(fs::path("a.d") / "README").empty()); // the name's, not its folder's
    CHECK(open_file::extension_of(util::path_from_utf8("evil\xEF\xBC\x8E" "bat")).empty()); // local::name's fullwidth period is no dot
}

TEST_CASE("programs, installers, scripts and shortcuts run whatever this PC says of them") {
    const Facts none{};
    for (const auto* ext : {".exe", ".com", ".bat", ".cmd", ".pif", ".scr", ".cpl", ".msc", ".msi", ".msp", ".lnk", ".url",
                            ".reg", ".hta", ".ps1", ".psm1", ".vbs", ".vbe", ".js", ".jse", ".wsf", ".wsh", ".jar",
                            ".appref-ms", ".application", ".msix", ".appx", ".diagcab", ".jnlp", ".a3x"}) {
        CAPTURE(ext);
        CHECK(open_file::runs_program(ext, none));
    }
    for (const auto* ext : {".txt", ".jpg", ".png", ".mkv", ".mp4", ".mp3", ".pdf", ".zip", ".7z", ".docx", ".srt", "", "."}) {
        CAPTURE(ext);
        CHECK_FALSE(open_file::runs_program(ext, none));
    }
}

TEST_CASE("PATHEXT's extensions and the types Windows calls unsafe run too") {
    const Facts py{.pathext = ".COM;.EXE; .Py ;PYW", .open_command = "", .dangerous = false};
    CHECK(open_file::runs_program(".py", py));
    CHECK(open_file::runs_program(".pyw", py)); // an entry without its dot
    CHECK_FALSE(open_file::runs_program(".pyc", py));
    CHECK(open_file::runs_program(".xyz", Facts{.pathext = "", .open_command = "", .dangerous = true}));
}

TEST_CASE("a type whose open command runs the file itself, or hands it to a script host, runs a program") {
    CHECK(runs(R"("%1" %*)"));
    CHECK(runs("%1 /S"));
    CHECK(runs(R"("%L")"));
    CHECK(runs(R"("%SystemRoot%\System32\WScript.exe" "%1" %*)"));
    CHECK(runs(R"(C:\Windows\System32\CScript.exe "%1" %*)"));
    CHECK(runs(R"(C:\Windows\SysWOW64\mshta.exe "%1" {1E460BD7-F1C3-4B2E-88BF-4E770A288AF5}%U %*)"));
    CHECK(runs(R"(regedit.exe "%1")"));
    CHECK(runs(R"("%SystemRoot%\System32\msiexec.exe" /i "%1" %*)"));
    CHECK(runs(R"(%SystemRoot%\system32\mmc.exe "%1" %*)"));
    CHECK(runs(R"("C:\Program Files\PowerShell\7\pwsh.exe" -File "%1")"));
    CHECK(runs(R"("C:\Program Files\Java\jre1.8.0_511\bin\javaw.exe" -jar "%1" %*)"));
    CHECK(runs(R"("C:\Users\u\AppData\Local\Programs\Python\Launcher\py.exe" "%L" %*)"));
    CHECK(runs(R"(C:\Program Files\Python 3.12\pythonw.exe "%1")")); // unquoted, with spaces
    CHECK(runs(R"("C:\Python312\python3.12.exe" "%1" %*)"));
    CHECK(runs(R"("C:\Program Files\AutoHotkey\v2\AutoHotkey64.exe" "%1" %*)"));
    CHECK(runs(R"(C:\Windows\System32\cmd.exe /c "%1")"));
    CHECK(runs(R"("C:\Program Files\Git\git-bash.exe" --no-cd "%L" %*)")); // Git for Windows' .sh
    CHECK(runs(R"("C:\Program Files\Java\jre1.8.0_511in\jp2launcher.exe" -securejws "%1")")); // .jnlp
    CHECK(runs(R"("C:\Program Files (x86)\AutoIt3\AutoIt3_x64.exe" "%1" %*)"));
    CHECK(runs(R"("C:\Tclin\wish86t.exe" "%1" %*)"));
}

TEST_CASE("a type whose open command shows the file in an app opens at once") {
    CHECK_FALSE(runs(""));
    CHECK_FALSE(runs(R"(%SystemRoot%\system32\NOTEPAD.EXE %1)"));
    CHECK_FALSE(runs(R"("C:\Program Files (x86)\K-Lite Codec Pack\MPC-HC64\mpc-hc64.exe" "%1")"));
    CHECK_FALSE(runs(R"("C:\Program Files\WinRAR\WinRAR.exe" "%1")"));
    CHECK_FALSE(runs(R"("C:\Program Files\Notepad++\notepad++.exe" "%1")"));
    // Images in the old Photo Viewer go through rundll32, which runs nothing of the file's.
    CHECK_FALSE(runs(R"("C:\Windows\System32\rundll32.exe" "C:\Program Files\Windows Photo Viewer\PhotoViewer.dll", ImageView_Fullscreen %1)"));
    // Names that only start like a host's.
    CHECK_FALSE(runs(R"(C:\tools\shower.exe "%1")"));
    CHECK_FALSE(runs(R"("C:\tools\cmdview.exe" "%1")"));
    CHECK_FALSE(runs(R"("C:\tools\node-viewer.exe" "%1")"));
}

#ifdef _WIN32
TEST_CASE("this PC's own facts: programs run, documents open") {
    const auto exe = open_file::facts_of(".exe");
    CHECK(exe.dangerous); // Windows' own list
    REQUIRE_FALSE(exe.open_command.empty());
    // Its "open" command alone (exefile: "%1" %*) says a program, outside every list.
    CHECK(runs(exe.open_command));
    CHECK(runs(open_file::facts_of(".bat").open_command));
    CHECK_FALSE(open_file::facts_of(".txt").dangerous);
    const auto unknown = open_file::facts_of(".grab-test-no-such-type");
    CHECK(unknown.open_command.empty());
    CHECK_FALSE(unknown.dangerous);

    const fs::path dir = fs::path("D:/dl") / "f";
    CHECK(open_file::runs_program(dir / "setup.EXE"));
    CHECK(open_file::runs_program(dir / ".bat"));
    CHECK(open_file::runs_program(dir / "run.cmd. "));
    CHECK(open_file::runs_program(dir / "get.ps1"));
    for (const auto* name : {"notes.txt", "photo.jpg", "shot.png", "e01.mkv", "clip.mp4", "song.mp3", "set.zip",
                             "paper.pdf", "README", "grab-test.grab-test-no-such-type"}) {
        CAPTURE(name);
        CHECK_FALSE(open_file::runs_program(dir / name));
    }
}

TEST_CASE("PATHEXT as the process has it, Windows' default without one") {
    {
        const test::ScopedEnv pathext("PATHEXT", ".COM;.EXE;.GRABTESTRUN");
        CHECK(open_file::runs_program(fs::path("tool.grabtestrun")));
    }
    const test::ScopedEnv none("PATHEXT", nullptr);
    CHECK(open_file::facts_of(".txt").pathext == ".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC");
    CHECK_FALSE(open_file::runs_program(fs::path("tool.grabtestrun")));
}
#endif

TEST_CASE("a finished file opens only while it is what grab wrote") {
    const auto dir = fs::temp_directory_path() / "grab-test-open-file";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const auto file = dir / "notes.txt";
    std::ofstream(file, std::ios::binary) << "hello";

    CHECK(open_file::check({.path = file, .size = 5}) == Found::ok);
    CHECK(open_file::check({.path = file, .size = 4}) == Found::changed); // edited since
    CHECK(open_file::check({.path = dir / "gone.txt", .size = 5}) == Found::gone);
    CHECK(open_file::check({.path = dir / "no" / "gone.txt", .size = 5}) == Found::gone);
    fs::create_directories(dir / "a folder.txt");
    CHECK(open_file::check({.path = dir / "a folder.txt", .size = 0}) == Found::changed);
    const auto empty = dir / "empty.bin";
    std::ofstream(empty, std::ios::binary).close();
    CHECK(open_file::check({.path = empty, .size = 0}) == Found::ok);
    fs::remove_all(dir, ec);
}
