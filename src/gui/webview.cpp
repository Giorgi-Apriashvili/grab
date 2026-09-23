#include "webview.hpp"

#include "resources.hpp"
#include "util.hpp"

#include <shellapi.h>
#include <shlwapi.h>
#include <wrl/event.h>

#include <string_view>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace grab::gui {

namespace {

constexpr std::wstring_view ui_origin = L"https://grab.ui";

// Frees a string WebView2 allocated with CoTaskMemAlloc.
struct CoString {
    LPWSTR p = nullptr;
    ~CoString() { CoTaskMemFree(p); }
    [[nodiscard]] std::wstring_view view() const { return p ? std::wstring_view(p) : std::wstring_view(); }
};

// Path part of a https://grab.ui/... URL, without query or fragment.
std::wstring_view ui_path(std::wstring_view uri) {
    if (!uri.starts_with(ui_origin)) return {};
    uri.remove_prefix(ui_origin.size());
    const auto cut = uri.find_first_of(L"?#");
    if (cut != std::wstring_view::npos) uri = uri.substr(0, cut);
    return uri.empty() ? std::wstring_view(L"/") : uri;
}

void open_externally(std::wstring_view uri) {
    // Only web links leave the app; anything else is ignored.
    if (uri.starts_with(L"https://") || uri.starts_with(L"http://")) {
        const std::wstring target(uri);
        ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

} // namespace

HRESULT WebViewHost::start(HWND hwnd, Options options, OnMessage on_message, OnReady on_ready) {
    hwnd_ = hwnd;
    options_ = std::move(options);
    on_message_ = std::move(on_message);
    on_ready_ = std::move(on_ready);

    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, options_.user_data_dir.empty() ? nullptr : options_.user_data_dir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) { return on_environment(result, env); })
            .Get());
    if (FAILED(hr)) fail(hr);
    return hr;
}

void WebViewHost::fail(HRESULT hr) {
    if (on_ready_) {
        auto cb = std::move(on_ready_);
        on_ready_ = nullptr;
        cb(hr);
    }
}

HRESULT WebViewHost::on_environment(HRESULT result, ICoreWebView2Environment* env) {
    if (FAILED(result) || env == nullptr) {
        fail(FAILED(result) ? result : E_FAIL);
        return S_OK;
    }
    env_ = env;
    const HRESULT hr = env_->CreateCoreWebView2Controller(
        hwnd_, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                   [this](HRESULT r, ICoreWebView2Controller* c) { return on_controller(r, c); })
                   .Get());
    if (FAILED(hr)) fail(hr);
    return S_OK;
}

HRESULT WebViewHost::on_controller(HRESULT result, ICoreWebView2Controller* controller) {
    if (FAILED(result) || controller == nullptr) {
        fail(FAILED(result) ? result : E_FAIL);
        return S_OK;
    }
    controller_ = controller;
    controller_->get_CoreWebView2(&webview_);
    set_background(options_.background);

    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(webview_->get_Settings(&settings))) {
        settings->put_AreDevToolsEnabled(options_.dev_tools ? TRUE : FALSE);
        settings->put_AreDefaultContextMenusEnabled(options_.dev_tools ? TRUE : FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        ComPtr<ICoreWebView2Settings3> settings3;
        if (SUCCEEDED(settings.As(&settings3))) {
            // No F5 reloads or Ctrl+P prints wiping the app's state in release builds.
            settings3->put_AreBrowserAcceleratorKeysEnabled(options_.dev_tools ? TRUE : FALSE);
        }
    }

    EventRegistrationToken token{};
    webview_->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                CoString json;
                if (SUCCEEDED(args->get_WebMessageAsJson(&json.p)) && on_message_) {
                    on_message_(util::to_utf8(json.view()));
                }
                return S_OK;
            })
            .Get(),
        &token);

    // Keep the window on the app: other links open in the default browser.
    webview_->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                CoString uri;
                args->get_Uri(&uri.p);
                if (!uri.view().starts_with(ui_origin)) {
                    args->put_Cancel(TRUE);
                    open_externally(uri.view());
                }
                return S_OK;
            })
            .Get(),
        &token);
    webview_->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                CoString uri;
                args->get_Uri(&uri.p);
                args->put_Handled(TRUE);
                open_externally(uri.view());
                return S_OK;
            })
            .Get(),
        &token);

    ComPtr<ICoreWebView2_3> webview3;
    if (!options_.dev_ui_dir.empty() && SUCCEEDED(webview_.As(&webview3))) {
        // Development: serve the UI straight from the source tree so edits need only a reload.
        webview3->SetVirtualHostNameToFolderMapping(L"grab.ui", options_.dev_ui_dir.c_str(),
                                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
    } else {
        // Normal: the UI files are resources inside the exe, served from memory.
        webview_->AddWebResourceRequestedFilter(L"https://grab.ui/*",
                                                COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        webview_->add_WebResourceRequested(
            Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                    ComPtr<ICoreWebView2WebResourceRequest> request;
                    if (FAILED(args->get_Request(&request))) return S_OK;
                    CoString uri;
                    request->get_Uri(&uri.p);
                    ComPtr<ICoreWebView2WebResourceResponse> response;
                    if (auto file = find_embedded(ui_path(uri.view()))) {
                        ComPtr<IStream> stream;
                        stream.Attach(SHCreateMemStream(reinterpret_cast<const BYTE*>(file->data.data()),
                                                        static_cast<UINT>(file->data.size())));
                        const std::wstring headers =
                            std::wstring(L"Content-Type: ") + file->content_type +
                            L"\r\nCache-Control: no-store";
                        env_->CreateWebResourceResponse(stream.Get(), 200, L"OK", headers.c_str(),
                                                        &response);
                    } else {
                        env_->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &response);
                    }
                    args->put_Response(response.Get());
                    return S_OK;
                })
                .Get(),
            &token);
    }

    RECT bounds{};
    GetClientRect(hwnd_, &bounds);
    controller_->put_Bounds(bounds);
    controller_->put_IsVisible(TRUE);
    const HRESULT hr = webview_->Navigate(L"https://grab.ui/index.html");
    if (on_ready_) {
        auto cb = std::move(on_ready_);
        on_ready_ = nullptr;
        cb(hr);
    }
    return S_OK;
}

void WebViewHost::resize(const RECT& bounds) {
    if (controller_) controller_->put_Bounds(bounds);
}

void WebViewHost::post_json(const std::string& json) {
    if (webview_) webview_->PostWebMessageAsJson(util::to_wide(json).c_str());
}

void WebViewHost::focus() {
    if (controller_) controller_->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

void WebViewHost::set_background(COLORREF color) {
    options_.background = color;
    ComPtr<ICoreWebView2Controller2> controller2;
    if (controller_ && SUCCEEDED(controller_.As(&controller2))) {
        controller2->put_DefaultBackgroundColor(
            COREWEBVIEW2_COLOR{255, GetRValue(color), GetGValue(color), GetBValue(color)});
    }
}

void WebViewHost::close() {
    if (controller_) controller_->Close();
    webview_.Reset();
    controller_.Reset();
    env_.Reset();
}

} // namespace grab::gui
