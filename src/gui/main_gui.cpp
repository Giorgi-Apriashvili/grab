// grab-gui: a WebView2 window over the same engine as the grab CLI.

#include "app.hpp"
#include "config.hpp"
#include "gui_state.hpp"
#include "preview_jobs.hpp"
#include "process.hpp"
#include "tray.hpp"
#include "update.hpp"
#include "util.hpp"
#include "webview.hpp"
#include "window_size.hpp"

#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)

#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <chrono>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

using namespace grab;

namespace {

constexpr const wchar_t* window_class = update::gui_window_class;
constexpr wchar_t instance_mutex[] = L"Local\\grab.gui.single-instance.";
constexpr wchar_t instance_slot[] = L"Local\\grab.gui.window-handle.";
constexpr UINT wm_post_json = WM_APP + 1; // lParam: std::string*
constexpr UINT wm_run = WM_APP + 2;       // lParam: std::function<void()>*
constexpr UINT wm_show = WM_APP + 3;      // from a second launch: bring the window back
constexpr UINT wm_quit = WM_APP + 4;      // App::Host::quit, once the call that asked has returned (G138)
constexpr UINT_PTR timer_tray = 1;        // every second: the tray tooltip and the speed schedule
constexpr UINT_PTR timer_exit = 2;        // fallback exit when a notification never reports back
constexpr UINT_PTR timer_flush = 3;       // every 250 ms: the queue's changes to the page (App::flush)
constexpr UINT id_show = 1;               // tray menu
constexpr UINT id_quit = 2;

struct Globals {
    HWND hwnd = nullptr;
    gui::WebViewHost webview;
    std::unique_ptr<gui::App> app;
    gui::TrayIcon tray;
    UINT wm_taskbar_created = 0;
    UINT wm_close_for_update = 0; // update::gui_close_request
    std::wstring tip;
    bool in_tray = false;         // closed while downloads ran: hidden, not quit
    bool exit_after_notice = false; // the queue drained in the tray: quit once the toast is gone
    std::uint64_t last_notice = 0;  // GetTickCount64() of the last notification
} g;

// Ignore "notification gone" events this soon after showing one: they belong to the
// notification it replaced, and the new one has not been seen yet.
constexpr std::uint64_t notice_min_ms = 2000;

bool may_exit_now() {
    return g.exit_after_notice && g.in_tray && g.app && !g.app->downloads_active() &&
           GetTickCount64() - g.last_notice >= notice_min_ms;
}

void notify(std::wstring_view title, std::wstring_view text, bool error) {
    g.last_notice = GetTickCount64();
    g.tray.notify(title, text, error);
}


bool system_uses_dark_theme() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return value == 0;
}

// Matches the page's --bg colors so there is no white flash before it paints.
COLORREF background_color(bool dark) { return dark ? RGB(0x1b, 0x1d, 0x21) : RGB(0xf6, 0xf7, 0xf9); }

void apply_theme(HWND hwnd) {
    const bool dark = system_uses_dark_theme();
    const BOOL on = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &on, sizeof(on));
    g.webview.set_background(background_color(dark));
}

void post_to_ui(UINT msg, void* payload, auto&& destroy) {
    if (g.hwnd == nullptr || !PostMessageW(g.hwnd, msg, 0, reinterpret_cast<LPARAM>(payload))) {
        destroy();
    }
}

void remember_placement(HWND hwnd) {
    if (!g.app) return;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(hwnd, &wp)) return;
    const RECT& r = wp.rcNormalPosition; // the restored size, even when maximized
    g.app->state().window = WindowPlacement{r.left, r.top, r.right - r.left, r.bottom - r.top,
                                            wp.showCmd == SW_SHOWMAXIMIZED};
    g.app->save_state();
}

void show_webview_error(HWND hwnd, HRESULT hr) {
    const bool missing_runtime = hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
    const std::wstring text =
        missing_runtime
            ? L"grab-gui needs the Microsoft Edge WebView2 Runtime, which is not installed.\n\n"
              L"Install it from https://go.microsoft.com/fwlink/p/?LinkId=2124703 and start "
              L"grab-gui again. Open the download page now?"
            : std::format(L"The WebView2 control could not start (error 0x{:08X}).", static_cast<unsigned long>(hr));
    const int answer = MessageBoxW(hwnd, text.c_str(), L"grab",
                                   (missing_runtime ? MB_YESNO : MB_OK) | MB_ICONERROR);
    if (missing_runtime && answer == IDYES) {
        ShellExecuteW(nullptr, L"open", L"https://go.microsoft.com/fwlink/p/?LinkId=2124703",
                      nullptr, nullptr, SW_SHOWNORMAL);
    }
    DestroyWindow(hwnd);
}

void show_window(HWND hwnd) {
    g.in_tray = false;
    g.exit_after_notice = false;
    KillTimer(hwnd, timer_exit);
    if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOW);
    if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
    SetForegroundWindow(hwnd);
}

// The last resort once grab-gui is leaving, its queue and settings saved: whatever still holds the
// exit after update::gui_exit_deadline, the process ends then. An installer's Restart Manager gives
// up on a program that takes more than about 30 s (and the update with it), and `grab update`
// waits for it.
void end_by_deadline() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            std::this_thread::sleep_for(update::gui_exit_deadline);
            TerminateProcess(GetCurrentProcess(), 0);
        }).detach();
    });
}

// Leaves now, even with downloads running: saved first (the window's place here, the rest in
// App::close), then every thread told to stop and every connection cut, so the joins in WM_DESTROY
// take moments (RCLONE-REPLACEMENT.md, Phase 9 chunk E).
void quit(HWND hwnd) {
    if (!g.in_tray) remember_placement(hwnd);
    if (g.app) g.app->close();
    end_by_deadline();
    DestroyWindow(hwnd);
}

// "grab" or "grab: 2 downloading, 43%".
std::wstring tray_tip() {
    if (!g.app) return L"grab";
    const auto s = g.app->summary();
    const int active = s.running + s.queued;
    if (active == 0 && s.paused == 0) return L"grab";
    std::wstring tip = L"grab:";
    if (active > 0) tip += L" " + std::to_wstring(active) + L" downloading";
    if (s.paused > 0) tip += std::wstring(active > 0 ? L"," : L"") + L" " + std::to_wstring(s.paused) + L" paused";
    if (s.total > 0) tip += L", " + std::to_wstring(s.bytes * 100 / s.total) + L"%";
    return tip;
}

void refresh_tray_tip() {
    std::wstring tip = tray_tip();
    if (tip != g.tip) {
        g.tip = tip;
        g.tray.set_tip(g.tip);
    }
}

// UI thread: a download finished or failed.
void on_download_finished(HWND hwnd, const std::string& name, bool ok, const std::string& error) {
    const bool in_front = IsWindowVisible(hwnd) && !IsIconic(hwnd) && GetForegroundWindow() == hwnd;
    if (!in_front) {
        const std::wstring wname = util::to_wide(name);
        notify(ok ? L"Downloaded" : L"Download failed", ok ? wname : wname + L"\n" + util::to_wide(error), !ok);
    }
    refresh_tray_tip();
    if (g.in_tray && g.app && !g.app->downloads_active()) {
        // Everything is done while the window is closed: leave once the notification has
        // been seen, or after a minute if Windows never reports back (e.g. do not disturb).
        g.exit_after_notice = true;
        SetTimer(hwnd, timer_exit, 60'000, nullptr);
    }
}

void show_tray_menu(HWND hwnd, POINT at) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, id_show, L"Show grab");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, id_quit, L"Quit");
    SetMenuDefaultItem(menu, id_show, FALSE);
    SetForegroundWindow(hwnd); // so the menu closes when clicking elsewhere
    const UINT cmd = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, at.x, at.y, hwnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    if (cmd == id_show) {
        show_window(hwnd);
    } else if (cmd == id_quit) {
        if (g.app && g.app->downloads_active() &&
            MessageBoxW(hwnd, L"Downloads are still running or queued.\n\nCancel them and quit?", L"grab",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
            return;
        }
        quit(hwnd);
    }
}

LRESULT on_tray_event(HWND hwnd, WPARAM wparam, LPARAM lparam) {
    switch (LOWORD(lparam)) {
    case NIN_SELECT:
    case NIN_KEYSELECT:
    case NIN_BALLOONUSERCLICK:
        show_window(hwnd);
        break;
    case WM_CONTEXTMENU:
        show_tray_menu(hwnd, POINT{GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam)});
        break;
    case NIN_BALLOONTIMEOUT:
    case NIN_BALLOONHIDE:
        if (may_exit_now()) quit(hwnd);
        break;
    default:
        break;
    }
    return 0;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == g.wm_taskbar_created && msg != 0) {
        g.tray.readd(); // Explorer restarted
        return 0;
    }
    if (msg == g.wm_close_for_update && msg != 0) {
        // `grab update` replaces grab-gui and starts it again afterwards: running and queued
        // downloads go on there.
        if (g.app) g.app->will_restart();
        quit(hwnd);
        return 0;
    }
    switch (msg) {
    case gui::TrayIcon::callback_message:
        return on_tray_event(hwnd, wparam, lparam);
    case wm_show:
        show_window(hwnd);
        return 0;
    case wm_quit:
        quit(hwnd);
        return 0;
    case WM_TIMER:
        if (wparam == timer_tray) {
            refresh_tray_tip();
            if (g.app) g.app->tick();
        } else if (wparam == timer_flush) {
            if (g.app) g.app->flush();
        } else if (wparam == timer_exit) {
            KillTimer(hwnd, timer_exit);
            if (may_exit_now()) quit(hwnd);
        }
        return 0;
    case WM_SIZE: {
        RECT bounds{};
        GetClientRect(hwnd, &bounds);
        g.webview.resize(bounds);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        const int dpi = static_cast<int>(GetDpiForWindow(hwnd));
        // Never more than the work area of the window's monitor: Windows holds even a maximized
        // window to this minimum, which would put its bottom under the taskbar on a small screen.
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        int work_w = 0;
        int work_h = 0;
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
            work_w = monitor.rcWork.right - monitor.rcWork.left;
            work_h = monitor.rcWork.bottom - monitor.rcWork.top;
        }
        info->ptMinTrackSize = {window_size::min_px(window_size::min_width, dpi, work_w),
                                window_size::min_px(window_size::min_height, dpi, work_h)};
        return 0;
    }
    case WM_SETFOCUS:
        g.webview.focus();
        return 0;
    case WM_SETTINGCHANGE:
        if (lparam != 0 && std::wstring_view(reinterpret_cast<const wchar_t*>(lparam)) == L"ImmersiveColorSet") {
            apply_theme(hwnd);
        }
        break;
    case wm_post_json: {
        std::unique_ptr<std::string> json(reinterpret_cast<std::string*>(lparam));
        g.webview.post_json(*json);
        return 0;
    }
    case wm_run: {
        std::unique_ptr<std::function<void()>> fn(reinterpret_cast<std::function<void()>*>(lparam));
        (*fn)();
        return 0;
    }
    case WM_CLOSE:
        if (g.app && g.app->downloads_active()) {
            // Downloads keep going in the tray; grab leaves by itself when they are done.
            remember_placement(hwnd);
            ShowWindow(hwnd, SW_HIDE);
            if (!g.in_tray) {
                g.in_tray = true;
                notify(L"grab is still downloading",
                       L"The downloads continue in the background, and grab closes when they finish. "
                       L"Click the grab icon to open it again.",
                       false);
            }
            return 0;
        }
        quit(hwnd);
        return 0;
    // Logoff, shutdown, or an installer closing grab-gui through the Restart Manager
    // (ENDSESSION_CLOSEAPP): agree, then exit at once (quit). Running downloads stop where they
    // are and keep their partial files; an installer update starts grab-gui again afterwards.
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wparam) quit(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, timer_tray);
        KillTimer(hwnd, timer_flush);
        KillTimer(hwnd, timer_exit);
        g.app.reset(); // joins the worker threads (told to stop by App::close) while the window still exists
        g.tray.remove();
        g.webview.close();
        g.hwnd = nullptr;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// A monitor's work area and DPI, for the window's size (window_size.hpp).
struct MonitorArea {
    window_size::Area work;
    int dpi = 96;
};

MonitorArea monitor_area(HMONITOR monitor) {
    MonitorArea out;
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (monitor != nullptr && GetMonitorInfoW(monitor, &info)) {
        out.work = {info.rcWork.left, info.rcWork.top, info.rcWork.right, info.rcWork.bottom};
    }
    UINT x = 0;
    UINT y = 0;
    const bool known = monitor != nullptr && SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y));
    out.dpi = static_cast<int>(known ? x : GetDpiForSystem());
    return out;
}

// Restores the saved placement if it is still on a connected monitor. A window an earlier grab
// saved smaller than today's default grows once (window_size::grown_once). Returns whether
// gui.json's layout changed, to be saved: from now on the window keeps the size it is given.
bool place_window(HWND hwnd, GuiState& state, int& show) {
    if (state.window) {
        const auto& w = *state.window;
        RECT r{w.x, w.y, w.x + w.width, w.y + w.height};
        if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL) != nullptr) {
            WINDOWPLACEMENT wp{};
            wp.length = sizeof(wp);
            wp.rcNormalPosition = r;
            wp.showCmd = SW_HIDE;
            SetWindowPlacement(hwnd, &wp);
            if (w.maximized) show = SW_SHOWMAXIMIZED;
            // Grown in screen coordinates (rcNormalPosition's are the workspace's), on the
            // monitor it is now on, at that monitor's DPI.
            RECT now{};
            if (state.layout < window_size::layout && GetWindowRect(hwnd, &now)) {
                const auto area = monitor_area(MonitorFromRect(&now, MONITOR_DEFAULTTONEAREST));
                const WindowPlacement before{now.left, now.top, now.right - now.left, now.bottom - now.top,
                                             w.maximized};
                const auto grown = window_size::grown_once(before, state.layout, area.work, area.dpi);
                if (grown != before && SetWindowPos(hwnd, nullptr, grown.x, grown.y, grown.width, grown.height,
                                                    SWP_NOZORDER | SWP_NOACTIVATE)) {
                    // Saved at once with the layout, so a window that doesn't close normally keeps it.
                    if (GetWindowPlacement(hwnd, &wp)) {
                        const RECT& n = wp.rcNormalPosition;
                        state.window =
                            WindowPlacement{n.left, n.top, n.right - n.left, n.bottom - n.top, before.maximized};
                    }
                }
            }
        }
    }
    if (state.layout >= window_size::layout) return false;
    state.layout = window_size::layout;
    return true;
}

// Whether `p` (the process --restart names) is the old window that started this one, before it is
// waited for and ended. Its PID may already be another program's, once the old window left before
// this one got here (or a hand-typed one): that one was created after this process, or runs
// another program.
bool is_old_window(HANDLE p) {
    FILETIME created{};
    FILETIME mine{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(p, &created, &exited, &kernel, &user) ||
        !GetProcessTimes(GetCurrentProcess(), &mine, &exited, &kernel, &user) ||
        CompareFileTime(&created, &mine) >= 0) {
        return false;
    }
    std::wstring image(32768, L'\0');
    auto size = static_cast<DWORD>(image.size());
    if (QueryFullProcessImageNameW(p, 0, image.data(), &size) == 0) return false;
    image.resize(size);
    const std::filesystem::path program(image);
    const auto name = util::to_lower(util::path_to_utf8(program.filename()));
    if (name != "grab-gui.exe" && name != "grab-gui.exe.old") return false;
    const auto here = util::self_exe_path().parent_path();
    std::error_code ec;
    return update::same_dir(util::path_to_utf8(program.parent_path()), util::path_to_utf8(here)) ||
           std::filesystem::equivalent(program.parent_path(), here, ec);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show) {
    // Before any thread starts: server secrets a 0.19 window left in the environment it handed on.
    proc::drop_inherited_server_variables();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // One window per configuration (%APPDATA%\grab, or GRAB_CONFIG's folder): a second launch
    // brings that window to the front, even from the tray. Its handle is published in a small
    // named shared-memory slot next to the mutex.
    const std::wstring key = util::to_wide(config_key(default_grab_config_path().parent_path()));
    HANDLE mutex = CreateMutexW(nullptr, TRUE, (std::wstring(instance_mutex) + key).c_str());
    bool already_running = mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS;
    // "--restart PID": started by an update's "Restart grab" while the old window (PID) is
    // closing. Wait until that process is gone, not merely its lock, so the programs it ran can
    // be deleted next, instead of handing over to it. It ends within update::gui_exit_deadline; one
    // still there after gui_exit_wait (a grab 0.20 window, which has no deadline and took a minute
    // with a login stuck) is ended here: it asked for this restart and saved its queue first.
    if (const auto restart = std::wstring_view(command_line).find(L"--restart"); restart != std::wstring_view::npos) {
        const DWORD old_pid = static_cast<DWORD>(std::wcstoul(command_line + restart + 9, nullptr, 10));
        constexpr DWORD access = SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION;
        if (HANDLE old = old_pid != 0 ? OpenProcess(access, FALSE, old_pid) : nullptr; old != nullptr) {
            const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(update::gui_exit_wait);
            if (is_old_window(old) && WaitForSingleObject(old, static_cast<DWORD>(wait.count())) == WAIT_TIMEOUT) {
                TerminateProcess(old, 1);
                WaitForSingleObject(old, 5000);
            }
            CloseHandle(old);
        }
        // Gone, it left the lock behind (abandoned); held, it is another window's, which takes over.
        if (already_running) {
            const DWORD waited = WaitForSingleObject(mutex, 5000);
            already_running = waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED;
        }
    }
    HANDLE slot = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(std::uint64_t),
                                     (std::wstring(instance_slot) + key).c_str());
    auto* published = slot != nullptr
                          ? static_cast<std::uint64_t*>(MapViewOfFile(slot, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(std::uint64_t)))
                          : nullptr;
    if (already_running) {
        if (published != nullptr && *published != 0) {
            HWND existing = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(*published));
            DWORD pid = 0;
            GetWindowThreadProcessId(existing, &pid);
            AllowSetForegroundWindow(pid); // this launch owns the foreground; hand it over
            PostMessageW(existing, wm_show, 0, 0);
        }
        return 0;
    }

    // An update replaced programs while they ran; the old ones can go now that this is the only
    // grab-gui (one restarted after a portable update waited for the old window above), and so can
    // the rclone.exe grab 0.20 and earlier shipped.
    update::clean_up_at_start();

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    const bool dark = system_uses_dark_theme();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = window_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, L"APP_ICON");
    if (wc.hIcon == nullptr) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = CreateSolidBrush(background_color(dark));
    wc.lpszClassName = window_class;
    RegisterClassExW(&wc);

    g.wm_close_for_update = RegisterWindowMessageW(update::gui_close_request);
    // Without a saved place (or with one on a monitor gone since), the window opens at the
    // default size in the middle of the primary monitor's work area.
    const auto primary = monitor_area(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    const auto initial = window_size::default_window(primary.work, primary.dpi);
    g.hwnd = CreateWindowExW(0, window_class, L"grab", WS_OVERLAPPEDWINDOW, initial.x, initial.y, initial.width,
                             initial.height, nullptr, nullptr, instance, nullptr);
    if (g.hwnd == nullptr) return 1;
    if (published != nullptr) *published = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(g.hwnd));

    gui::App::Host host;
    host.hwnd = g.hwnd;
    host.post = [](std::string json) {
        auto* payload = new std::string(std::move(json));
        post_to_ui(wm_post_json, payload, [payload] { delete payload; });
    };
    host.defer = [](std::function<void()> fn) {
        auto* payload = new std::function<void()>(std::move(fn));
        post_to_ui(wm_run, payload, [payload] { delete payload; });
    };
    host.on_finished = [defer = host.defer](const std::string& name, bool ok, const std::string& error) {
        defer([name, ok, error] {
            if (g.hwnd != nullptr) on_download_finished(g.hwnd, name, ok, error);
        });
    };
    // Posted: App's teardown must not run inside the App call that asked for it (review G138).
    host.quit = [] {
        if (g.hwnd != nullptr) PostMessageW(g.hwnd, wm_quit, 0, 0);
    };
    g.app = std::make_unique<gui::App>(std::move(host), default_grab_config_path(),
                                       state_dir(default_grab_config_path()) / "gui.json");

    if (place_window(g.hwnd, g.app->state(), show)) g.app->save_state();
    apply_theme(g.hwnd);
    ShowWindow(g.hwnd, show);
    UpdateWindow(g.hwnd);

    // Lets an installer's Restart Manager start grab-gui again after an update; not after a
    // crash, hang or reboot.
    RegisterApplicationRestart(L"", RESTART_NO_CRASH | RESTART_NO_HANG | RESTART_NO_REBOOT);

    g.wm_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    auto icon = [&](int metric) {
        const int size = GetSystemMetricsForDpi(metric, GetDpiForWindow(g.hwnd));
        return static_cast<HICON>(LoadImageW(instance, L"APP_ICON", IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    };
    g.tip = L"grab";
    g.tray.add(g.hwnd, icon(SM_CXSMICON), icon(SM_CXICON), g.tip);
    SetTimer(g.hwnd, timer_tray, 1000, nullptr);
    SetTimer(g.hwnd, timer_flush, 250, nullptr);

    gui::WebViewHost::Options options;
    // %LOCALAPPDATA% read as UTF-8 (G140): read in the ANSI code page, a profile folder such as
    // C:\Users\გიორგი became C:\Users\áƒ’..., which can't be created, and the window never opened.
    options.user_data_dir = (util::local_data_home() / "grab" / "WebView2").wstring();
    options.dev_ui_dir = util::to_wide(util::getenv_utf8("GRAB_UI_DIR").value_or(""));
#ifndef NDEBUG
    options.dev_tools = true;
#endif
    options.background = background_color(dark);
    // The viewer's previews come from the app, which answers on a worker thread.
    options.on_preview = [](const std::string& url, std::function<void(gui::WebResource)> reply) {
        if (g.app) g.app->preview_request(url, std::move(reply));
        else reply(preview::failed(503, "not ready"));
    };
    options.defer = [](std::function<void()> fn) {
        auto* payload = new std::function<void()>(std::move(fn));
        post_to_ui(wm_run, payload, [payload] { delete payload; });
    };
    g.webview.start(
        g.hwnd, std::move(options), [](const std::string& json) {
            if (g.app) g.app->on_message(json);
        },
        [](HRESULT hr) {
            if (FAILED(hr)) show_webview_error(g.hwnd, hr);
        });

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    if (published != nullptr) UnmapViewOfFile(published);
    if (slot != nullptr) CloseHandle(slot);
    if (mutex != nullptr) CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
