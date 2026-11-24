#pragma once

#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)

// WIN32_LEAN_AND_MEAN leaves COM out of windows.h; WebView2.h needs it (the `interface`
// keyword and IUnknown), so it must come first.
#include <objbase.h> // IWYU pragma: keep

#include <WebView2.h>
#include <wrl/client.h>

#include "preview_jobs.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace grab::gui {

// An answer to a request the page made, e.g. for a preview: the status, the content type, extra
// response headers ("Name: value" lines) and the body, held once (preview_jobs.hpp).
using WebResource = preview::Answer;

// Hosts the WebView2 control that renders the UI. All methods run on the UI thread.
class WebViewHost {
public:
    struct Options {
        std::wstring user_data_dir;   // browser profile (cache etc.), under %LOCALAPPDATA%
        std::wstring dev_ui_dir;      // non-empty: serve the UI from this folder (live edits)
        bool dev_tools = false;       // DevTools, context menu, browser accelerator keys
        COLORREF background = RGB(255, 255, 255); // shown until the page paints
        // The page's prefers-color-scheme (Settings → Appearance), set before it loads: AUTO
        // follows Windows.
        COREWEBVIEW2_PREFERRED_COLOR_SCHEME color_scheme = COREWEBVIEW2_PREFERRED_COLOR_SCHEME_AUTO;
        // The WebView2 profile in user_data_dir: empty is the Default one. A configuration other
        // than the default gets its own, as the colour scheme belongs to the profile.
        std::wstring profile_name;
        // Requests for https://grab.ui/preview/...: `reply` answers, later and from any thread. It
        // holds the request's id, never a WebView2 object (review G135): the request waits in the
        // host until the answer reaches the UI thread.
        std::function<void(const std::string& url, std::function<void(WebResource)> reply)> on_preview;
        std::function<void(std::function<void()>)> defer; // any thread: run on the UI thread
    };
    // A message from grab's page, and the paths of the files it posted beside it (a drop's Files,
    // UPLOADS.md 5.10): only the host gives paths, never the page's text.
    using OnMessage = std::function<void(const std::string& json, std::vector<std::filesystem::path> paths)>;
    // Called once: S_OK when the UI is navigating, or the failure (e.g. no WebView2 runtime).
    using OnReady = std::function<void(HRESULT)>;

    HRESULT start(HWND hwnd, Options options, OnMessage on_message, OnReady on_ready);
    void resize(const RECT& bounds);
    void post_json(const std::string& json);
    void focus();
    void set_background(COLORREF color);
    // The page's colours, its scrollbars and form controls follow at once, with no reload. A
    // WebView2 Runtime older than the profile's setting (ICoreWebView2_13) keeps following Windows.
    void set_color_scheme(COREWEBVIEW2_PREFERRED_COLOR_SCHEME scheme);
    // False once the runtime refused the colour scheme: the page follows Windows then, and so
    // must the title bar and the background.
    [[nodiscard]] bool follows_theme() const { return !color_scheme_failed_; }
    void close();
    [[nodiscard]] bool ready() const { return webview_ != nullptr; }

private:
    HRESULT on_environment(HRESULT result, ICoreWebView2Environment* env);
    HRESULT on_controller(HRESULT result, ICoreWebView2Controller* controller);
    void fail(HRESULT hr);
    // A web link from a preview: grab asks (showing where it really goes), then opens it in the
    // default browser. Anything else is dropped.
    void offer_link(std::wstring uri);
    // The answer to request `id` (UI thread): its response, over a stream of the shared body.
    void complete(std::uint64_t id, const WebResource& r);
    // grab's page is gone (its renderer ended, or an error page came in its place): it is opened
    // again at once, and once it says "init" it is told why ("pageRestored"), so it can say so and
    // go back where it was. At most restore_limit times a minute, so a page that can't load doesn't
    // reload forever (WebView2's own page with its Refresh button stays then).
    void restore_page(const char* why);

    HWND hwnd_ = nullptr;
    bool asking_ = false;
    bool color_scheme_failed_ = false; // set_color_scheme: the runtime has no ICoreWebView2_13
    std::string restored_;                                  // why the page was opened again, until it says "init"
    std::vector<std::chrono::steady_clock::time_point> restores_; // when, the last minute's
    Options options_;
    OnMessage on_message_;
    OnReady on_ready_;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> env_;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller_;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview_;
    // Requests answered later, by id: their deferral and arguments stay here, on the UI thread,
    // and are released here, whichever thread answers (review G135).
    struct Deferred {
        Microsoft::WRL::ComPtr<ICoreWebView2Deferral> deferral;
        Microsoft::WRL::ComPtr<ICoreWebView2WebResourceRequestedEventArgs> args;
    };
    std::map<std::uint64_t, Deferred> deferred_;
    std::uint64_t next_request_ = 0;
};

} // namespace grab::gui
