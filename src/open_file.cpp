#include "open_file.hpp"

#include "util.hpp"

#include <algorithm>
#include <array>
#include <system_error>

#ifdef _WIN32
#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)
#include <shlwapi.h> // IWYU pragma: keep (AssocQueryStringW, AssocIsDangerous)
#endif

namespace grab::open_file {

namespace fs = std::filesystem;

namespace {

// PATHEXT as Windows sets it, for a process started without one.
constexpr std::string_view default_pathext = ".COM;.EXE;.BAT;.CMD;.VBS;.VBE;.JS;.JSE;.WSF;.WSH;.MSC";

// Types whose "open" runs something, whatever PATHEXT and the registry say: programs, screen
// savers and Control Panel items, management consoles, installers and update or app packages,
// shortcuts (to anything, settings pages too), registry files, HTML applications, PowerShell,
// Windows Script Host and scriptlets, Java archives and Web Start apps, ClickOnce applications,
// troubleshooting packs, Excel add-ins (DLLs), compiled AutoIt scripts. Shell scripts (.sh, .bash)
// aren't here: they often open in an editor; when their "open" runs them, the host check below
// catches it.
constexpr auto programs = std::to_array<std::string_view>(
    {".exe",  ".com",          ".bat",         ".cmd",       ".pif",        ".scr",        ".cpl",  ".msc",
     ".msi",  ".msp",          ".msu",         ".appx",      ".appxbundle", ".msix",       ".msixbundle",
     ".appinstaller",          ".lnk",         ".url",       ".scf",        ".settingcontent-ms",   ".reg",
     ".hta",  ".ps1",          ".psm1",        ".psc1",      ".vbs",        ".vbe",        ".vb",   ".js",
     ".jse",  ".wsf",          ".wsh",         ".ws",        ".wsc",        ".sct",        ".jar",  ".appref-ms",
     ".application",           ".xbap",        ".gadget",    ".diagcab",    ".xll",        ".jnlp", ".a3x"});

// Programs that run the file they are given as a script or a package: an "open" command that
// starts one of these runs the file, though the file isn't a program itself (.py with Python).
constexpr auto hosts = std::to_array<std::string_view>(
    {"wscript", "cscript", "mshta", "powershell", "pwsh", "cmd", "regedit", "regedt32", "msiexec", "wusa", "mmc", "hh",
     "java", "javaw", "javaws", "jp2launcher", "py", "pyw", "perl", "ruby", "rubyw", "node", "php", "wsl", "bash",
     "sh", "git-bash", "mintty", "zsh", "ksh", "csh", "tcsh", "dash", "busybox"});
// ... and those whose names start so: python3.12.exe, pythonw.exe, AutoHotkey64.exe, AutoIt3_x64.exe,
// tclsh86t.exe, wish86t.exe.
constexpr auto host_prefixes = std::to_array<std::string_view>({"python", "autohotkey", "autoit3", "tclsh", "wish"});

// The program an "open" command starts, lower case, without its folder or ".exe": "wscript" for
// "%SystemRoot%\System32\WScript.exe" "%1" %*, "%1" for "%1" %* (the file itself).
std::string program_of(std::string_view command) {
    const std::string c = util::to_lower(util::trim(command));
    std::string_view first = c;
    if (first.starts_with('"')) {
        first.remove_prefix(1);
        first = first.substr(0, first.find('"'));
    } else {
        // Unquoted: "%1" or "%L" alone, else up to its ".exe" ("C:\Program Files\x\y.exe %1"),
        // else up to the first space.
        const auto token = first.substr(0, first.find(' '));
        const auto exe = first.find(".exe");
        first = token == "%1" || token == "%l" || exe == std::string_view::npos ? token : first.substr(0, exe + 4);
    }
    if (const auto slash = first.find_last_of("\\/"); slash != std::string_view::npos) first.remove_prefix(slash + 1);
    if (first.ends_with(".exe")) first.remove_suffix(4);
    return std::string(first);
}

} // namespace

std::string extension_of(const fs::path& file) {
    std::string name = util::path_to_utf8(file.filename());
    while (!name.empty() && (name.back() == '.' || name.back() == ' ')) name.pop_back();
    const auto dot = name.rfind('.');
    return dot == std::string::npos ? std::string() : util::to_lower(std::string_view(name).substr(dot));
}

bool runs_program(std::string_view ext, const Facts& facts) {
    if (ext.size() < 2 || ext.front() != '.') return false; // no extension: Windows asks which app
    if (std::ranges::contains(programs, ext)) return true;
    for (auto& e : util::split(facts.pathext, ';')) {
        if (!e.starts_with('.')) e.insert(0, ".");
        if (util::to_lower(e) == ext) return true;
    }
    if (facts.dangerous) return true;
    if (facts.open_command.empty()) return false;
    const auto program = program_of(facts.open_command);
    return program == "%1" || program == "%l" || std::ranges::contains(hosts, program) ||
           std::ranges::any_of(host_prefixes, [&](std::string_view p) { return program.starts_with(p); });
}

Facts facts_of(std::string_view ext) {
    Facts facts;
    facts.pathext = util::getenv_utf8("PATHEXT").value_or(std::string(default_pathext));
#ifdef _WIN32
    if (ext.size() < 2) return facts;
    const std::wstring type = util::to_wide(ext);
    facts.dangerous = AssocIsDangerous(type.c_str()) != FALSE;
    // The command, as the shell would run it for "open" (an unknown type has none). With no
    // buffer, the call gives the length it needs, its terminating null included.
    constexpr ASSOCF flags = ASSOCF_NOTRUNCATE | ASSOCF_INIT_IGNOREUNKNOWN;
    DWORD size = 0;
    if (AssocQueryStringW(flags, ASSOCSTR_COMMAND, type.c_str(), L"open", nullptr, &size) == S_FALSE && size > 1) {
        std::wstring command(size, L'\0');
        if (SUCCEEDED(AssocQueryStringW(flags, ASSOCSTR_COMMAND, type.c_str(), L"open", command.data(), &size))) {
            command.resize(std::min<std::size_t>(command.find(L'\0'), command.size()));
            facts.open_command = util::to_utf8(command);
        }
    }
#endif
    return facts;
}

bool runs_program(const fs::path& file) {
    const auto ext = extension_of(file);
    return runs_program(ext, facts_of(ext));
}

Found check(const Finished& f) {
    std::error_code ec;
    const auto status = fs::symlink_status(f.path, ec);
    if (ec || status.type() == fs::file_type::not_found) return Found::gone;
    if (status.type() != fs::file_type::regular) return Found::changed; // a folder or a link in its place
    const auto size = fs::file_size(f.path, ec);
    if (ec) return Found::gone;
    return size == f.size ? Found::ok : Found::changed;
}

} // namespace grab::open_file
