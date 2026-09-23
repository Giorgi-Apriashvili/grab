// grab-gui: a WebView2 window over the same engine as the grab CLI.

#include "app.hpp"
#include "config.hpp"
#include "gui_state.hpp"
#include "util.hpp"
#include "webview.hpp"

#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)

#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>

#include <functional>
#include <memory>
#include <string>

using namespace grab;

namespace {

constexpr wchar_t window_class[] = L"grab.gui.window";
constexpr wchar_t instance_mutex[] = L"Local\\grab.gui.single-instance";
constexpr UINT wm_post_json = WM_APP + 1; // lParam: std::string*
constexpr UINT wm_run = WM_APP + 2;       // lParam: std::function<void()>*

struct Globals {
    HWND hwnd = nullptr;
    gui::WebViewHost webview;
    std::unique_ptr<gui::App> app;
} g;

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

LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
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
        if (g.app && g.app->downloads_active() &&
            MessageBoxW(hwnd, L"Downloads are still running or queued.\n\nCancel them and quit?",
                        L"grab", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
            return 0;
        }
        remember_placement(hwnd);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        g.app.reset(); // cancels and joins worker threads while the window still exists
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

    // One window: a second launch brings the first to the front.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, instance_mutex);
    if (mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(window_class, nullptr)) {
            if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
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
    g.app = std::make_unique<gui::App>(std::move(host), default_grab_config_path(), default_gui_state_path());

    place_window(g.hwnd, g.app->state(), show);
    apply_theme(g.hwnd);
    ShowWindow(g.hwnd, show);
    UpdateWindow(g.hwnd);

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
    if (mutex != nullptr) CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
