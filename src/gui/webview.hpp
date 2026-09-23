#pragma once

#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)

// WIN32_LEAN_AND_MEAN leaves COM out of windows.h; WebView2.h needs it (the `interface`
// keyword and IUnknown), so it must come first.
#include <objbase.h> // IWYU pragma: keep

#include <WebView2.h>
#include <wrl/client.h>

#include <functional>
#include <string>

namespace grab::gui {

// Hosts the WebView2 control that renders the UI. All methods run on the UI thread.
class WebViewHost {
public:
    struct Options {
        std::wstring user_data_dir;   // browser profile (cache etc.), under %LOCALAPPDATA%
        std::wstring dev_ui_dir;      // non-empty: serve the UI from this folder (live edits)
        bool dev_tools = false;       // DevTools, context menu, browser accelerator keys
        COLORREF background = RGB(255, 255, 255); // shown until the page paints
    };
    using OnMessage = std::function<void(const std::string& json)>;
    // Called once: S_OK when the UI is navigating, or the failure (e.g. no WebView2 runtime).
    using OnReady = std::function<void(HRESULT)>;

    HRESULT start(HWND hwnd, Options options, OnMessage on_message, OnReady on_ready);
    void resize(const RECT& bounds);
    void post_json(const std::string& json);
    void focus();
    void set_background(COLORREF color);
    void close();
    [[nodiscard]] bool ready() const { return webview_ != nullptr; }

private:
    HRESULT on_environment(HRESULT result, ICoreWebView2Environment* env);
    HRESULT on_controller(HRESULT result, ICoreWebView2Controller* controller);
    void fail(HRESULT hr);

    HWND hwnd_ = nullptr;
    Options options_;
    OnMessage on_message_;
    OnReady on_ready_;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> env_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
};

} // namespace grab::gui
