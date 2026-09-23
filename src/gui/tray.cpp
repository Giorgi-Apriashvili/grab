#include "tray.hpp"

#include <cwchar>

namespace grab::gui {

namespace {

template <std::size_t N>
void copy_text(wchar_t (&dst)[N], std::wstring_view src) {
    // Truncates to fit; the shell shows at most N - 1 characters anyway.
    const std::size_t n = src.size() < N ? src.size() : N - 1;
    std::wmemcpy(dst, src.data(), n);
    dst[n] = L'\0';
}

} // namespace

bool TrayIcon::add(HWND hwnd, HICON icon, HICON large_icon, std::wstring_view tip) {
    data_ = {};
    data_.cbSize = sizeof(data_);
    data_.hWnd = hwnd;
    data_.uID = 1;
    data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data_.uCallbackMessage = callback_message;
    data_.hIcon = icon;
    copy_text(data_.szTip, tip);
    large_icon_ = large_icon;
    return readd();
}

bool TrayIcon::readd() {
    if (data_.hWnd == nullptr) return false;
    NOTIFYICONDATAW d = data_;
    d.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    added_ = Shell_NotifyIconW(NIM_ADD, &d) != FALSE;
    if (added_) {
        d.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &d);
    }
    return added_;
}

void TrayIcon::set_tip(std::wstring_view tip) {
    copy_text(data_.szTip, tip);
    if (!added_) return;
    NOTIFYICONDATAW d = data_;
    d.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &d);
}

void TrayIcon::notify(std::wstring_view title, std::wstring_view text, bool error) {
    if (!added_) return;
    NOTIFYICONDATAW d = data_;
    d.uFlags = NIF_INFO;
    copy_text(d.szInfoTitle, title);
    copy_text(d.szInfo, text);
    d.dwInfoFlags = NIIF_RESPECT_QUIET_TIME;
    if (error) {
        d.dwInfoFlags |= NIIF_ERROR;
    } else if (large_icon_ != nullptr) {
        d.dwInfoFlags |= NIIF_USER | NIIF_LARGE_ICON;
        d.hBalloonIcon = large_icon_;
    } else {
        d.dwInfoFlags |= NIIF_INFO;
    }
    Shell_NotifyIconW(NIM_MODIFY, &d);
}

void TrayIcon::remove() {
    if (!added_) return;
    NOTIFYICONDATAW d = data_;
    Shell_NotifyIconW(NIM_DELETE, &d);
    added_ = false;
}

} // namespace grab::gui
