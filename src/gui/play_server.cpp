#include "play_server.hpp"

#include "process.hpp"
#include "util.hpp"

#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)
#include <shlwapi.h> // AssocQueryStringW

#include <filesystem>
#include <optional>
#include <system_error>
#include <utility>

namespace grab::gui {

std::expected<std::string, std::string> open_with_player(std::string_view ext, const std::string& url) {
    std::wstring command;
#ifndef NDEBUG
    // Tests: a stand-in for the player, so none opens on the screen.
    if (const auto test = util::getenv_utf8("GRAB_TEST_PLAYER"); test && !test->empty()) {
        command = L"\"" + util::to_wide(*test) + L"\" \"%1\"";
    }
#endif
    const std::wstring dot = L"." + util::to_wide(ext);
    if (command.empty()) {
        // What Windows runs to open such a file: "C:\...\mpc-hc64.exe" "%1", say.
        DWORD length = 0;
        AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_COMMAND, dot.c_str(), L"open", nullptr, &length);
        if (length > 1) {
            command.resize(length);
            if (AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_COMMAND, dot.c_str(), L"open", command.data(),
                                  &length) == S_OK) {
                command.resize(length > 0 ? length - 1 : 0);
            } else {
                command.clear();
            }
        }
    }
    // A Store app opens files through a handler, not a command line that takes an address.
    const bool store_app = command.find(L"\\WindowsApps\\") != std::wstring::npos ||
                           command.find(L"DelegateExecute") != std::wstring::npos;
    if (command.empty() || store_app) {
        return util::failf("Windows opens .{} files with {}, which can't play a stream. Make a desktop player such as "
                           "VLC or MPC-HC the default for .{} files, or download it.",
                           ext, command.empty() ? "no program" : "a Store app", ext);
    }
    const std::wstring wide_url = util::to_wide(url);
    std::wstring line = command;
    bool placed = false;
    for (const std::wstring_view mark : {L"%1", L"%L", L"%l"}) {
        for (auto at = line.find(mark); at != std::wstring::npos; at = line.find(mark, at + wide_url.size())) {
            line.replace(at, mark.size(), wide_url);
            placed = true;
        }
    }
    if (!placed) line += L" \"" + wide_url + L"\"";
    for (auto at = line.find(L"%*"); at != std::wstring::npos; at = line.find(L"%*")) line.erase(at, 2);

    // The program part of the command. A bare name ("vlc.exe %1") is looked up as grab's own
    // programs are, never in the current folder (G3); a path stays as Windows reads it.
    std::wstring program = command;
    if (program.starts_with(L'"')) program = program.substr(1, program.find(L'"', 1) - 1);
    else program = program.substr(0, program.find(L' '));
    std::optional<std::filesystem::path> found;
    if (const std::filesystem::path p(program); !p.has_parent_path() && !p.has_root_name()) {
        auto where = proc::find_program(util::to_utf8(program));
        if (!where) return util::failf("cannot start the player: {}", where.error());
        found = std::move(*where);
    }

    // A program of someone else's that may run for hours: none of rclone's settings or secrets.
    std::wstring environment = proc::environment_without_rclone();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(found ? found->c_str() : nullptr, line.data(), nullptr, nullptr, FALSE,
                        CREATE_DEFAULT_ERROR_MODE | CREATE_UNICODE_ENVIRONMENT, environment.data(), nullptr, &si, &pi)) {
        return util::failf("cannot start the player: {}", std::system_category().message(static_cast<int>(GetLastError())));
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    // Its name, for the window.
    return util::path_to_utf8(std::filesystem::path(program).filename());
}

} // namespace grab::gui
