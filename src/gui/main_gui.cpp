// grab-gui: a WebView2 window over the same engine as the grab CLI.

#include "app.hpp"
#include "config.hpp"
#include "gui_state.hpp"
#include "tray.hpp"
#include "util.hpp"
#include "webview.hpp"

#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)

#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <windowsx.h>

#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

using namespace grab;

namespace {

constexpr wchar_t window_class[] = L"grab.gui.window";
constexpr wchar_t instance_mutex[] = L"Local\\grab.gui.single-instance.";
constexpr wchar_t instance_slot[] = L"Local\\grab.gui.window-handle.";
constexpr UINT wm_post_json = WM_APP + 1; // lParam: std::string*
constexpr UINT wm_run = WM_APP + 2;       // lParam: std::function<void()>*
constexpr UINT wm_show = WM_APP + 3;      // from a second launch: bring the window back
constexpr UINT_PTR timer_tray = 1;        // refreshes the tray tooltip
constexpr UINT_PTR timer_exit = 2;        // fallback exit when a notification never reports back
constexpr UINT id_show = 1;               // tray menu
constexpr UINT id_quit = 2;

struct Globals {
    HWND hwnd = nullptr;
    gui::WebViewHost webview;
    std::unique_ptr<gui::App> app;
    gui::TrayIcon tray;
    UINT wm_taskbar_created = 0;
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

// A short stable name for a configuration folder (FNV-1a of its lower-cased path), so each
// configuration gets its own single-instance lock.
std::wstring instance_key(const std::filesystem::path& config_dir) {
    std::uint64_t h = 1469598103934665603ULL;
    for (const wchar_t ch : util::to_wide(util::to_lower(util::path_to_utf8(config_dir)))) {
        h = (h ^ static_cast<std::uint64_t>(ch)) * 1099511628211ULL;
    }
    wchar_t buf[17];
    swprintf(buf, 17, L"%016llx", static_cast<unsigned long long>(h));
    return buf;
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
            : L"The WebView2 control could not start (error 0x" +
                  std::to_wstring(static_cast<unsigned long>(hr)) + L").";
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

void quit(HWND hwnd) {
    if (!g.in_tray) remember_placement(hwnd);
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
    switch (msg) {
    case gui::TrayIcon::callback_message:
        return on_tray_event(hwnd, wparam, lparam);
    case wm_show:
        show_window(hwnd);
        return 0;
    case WM_TIMER:
        if (wparam == timer_tray) {
            refresh_tray_tip();
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
        const UINT dpi = GetDpiForWindow(hwnd);
        info->ptMinTrackSize = {MulDiv(760, static_cast<int>(dpi), 96), MulDiv(480, static_cast<int>(dpi), 96)};
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
    // (ENDSESSION_CLOSEAPP): agree, then exit cleanly. Downloads are cancelled and their
    // partial files removed; an installer update starts grab-gui again afterwards.
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wparam) quit(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, timer_tray);
        KillTimer(hwnd, timer_exit);
        g.app.reset(); // cancels and joins worker threads while the window still exists
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

// Restores the saved placement if it is still on a connected monitor.
void place_window(HWND hwnd, const GuiState& state, int& show) {
    if (!state.window) return;
    const auto& w = *state.window;
    RECT r{w.x, w.y, w.x + w.width, w.y + w.height};
    if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL) == nullptr) return;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof(wp);
    wp.rcNormalPosition = r;
    wp.showCmd = SW_HIDE;
    SetWindowPlacement(hwnd, &wp);
    if (w.maximized) show = SW_SHOWMAXIMIZED;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // One window per configuration (%APPDATA%\grab, or GRAB_CONFIG's folder): a second launch
    // brings that window to the front, even from the tray. Its handle is published in a small
    // named shared-memory slot next to the mutex.
    const std::wstring key = instance_key(default_grab_config_path().parent_path());
    HANDLE mutex = CreateMutexW(nullptr, TRUE, (std::wstring(instance_mutex) + key).c_str());
    const bool already_running = mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS;
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

    const UINT dpi = GetDpiForSystem();
    g.hwnd = CreateWindowExW(0, window_class, L"grab", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             MulDiv(1100, static_cast<int>(dpi), 96), MulDiv(720, static_cast<int>(dpi), 96),
                             nullptr, nullptr, instance, nullptr);
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
    g.app = std::make_unique<gui::App>(std::move(host), default_grab_config_path(), default_gui_state_path());

    place_window(g.hwnd, g.app->state(), show);
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

    gui::WebViewHost::Options options;
    options.user_data_dir = (std::filesystem::path(util::getenv_utf8("LOCALAPPDATA").value_or(".")) / "grab" / "WebView2").wstring();
    options.dev_ui_dir = util::to_wide(util::getenv_utf8("GRAB_UI_DIR").value_or(""));
#ifndef NDEBUG
    options.dev_tools = true;
#endif
    options.background = background_color(dark);
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
