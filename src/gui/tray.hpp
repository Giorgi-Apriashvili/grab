#pragma once

#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)

#include <shellapi.h>

#include <string>
#include <string_view>

namespace grab::gui {

// grab's notification-area icon. Its events arrive at the window as `callback_message`
// (NOTIFYICON_VERSION_4: LOWORD(lParam) is the event, e.g. NIN_SELECT, WM_CONTEXTMENU,
// NIN_BALLOONUSERCLICK). UI thread only.
class TrayIcon {
public:
    static constexpr UINT callback_message = WM_APP + 10;

    TrayIcon() = default;
    ~TrayIcon() { remove(); }
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool add(HWND hwnd, HICON icon, HICON large_icon, std::wstring_view tip);
    bool readd(); // after Explorer restarts (the "TaskbarCreated" message)
    void set_tip(std::wstring_view tip);
    // A notification; Windows 10/11 show it as a toast. Quiet hours are respected.
    void notify(std::wstring_view title, std::wstring_view text, bool error);
    void remove();

private:
    NOTIFYICONDATAW data_{};
    HICON large_icon_ = nullptr;
    bool added_ = false;
};

} // namespace grab::gui
