#include "webview.hpp"

#include "body_stream.hpp"
#include "json.hpp"
#include "resources.hpp"
#include "ui_origin.hpp"
#include "util.hpp"

#include <shellapi.h>
#include <shlwapi.h>
#include <wrl/event.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string_view>
#include <utility>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace grab::gui {

namespace {

std::wstring reason(int status) {
    switch (status) {
    case 200: return L"OK";
    case 404: return L"Not Found";
    case 413: return L"Too Large";
    case 415: return L"Unsupported";
    case 503: return L"Unavailable";
    default: return status >= 500 ? L"Error" : L"Bad Request";
    }
}

constexpr const wchar_t* page_url = L"https://grab.ui/index.html";
// How often a minute grab's page is opened again after it was lost (WebViewHost::restore_page).
constexpr std::size_t restore_limit = 3;

// Frees a string WebView2 allocated with CoTaskMemAlloc.
struct CoString {
    LPWSTR p = nullptr;
    ~CoString() { CoTaskMemFree(p); }
    [[nodiscard]] std::wstring_view view() const { return p ? std::wstring_view(p) : std::wstring_view(); }
};

} // namespace

void WebViewHost::offer_link(std::wstring uri) {
    if (!ui_origin::is_web_link(uri) || !options_.defer) return;
    // After the event handler has returned: a dialog's message loop inside it could re-enter WebView2.
    options_.defer([this, uri = std::move(uri)] {
        if (asking_) return; // one question at a time; more clicks while it shows are dropped
        asking_ = true;
        const std::wstring text = L"A link in the preview goes to:\n\n" + ui_origin::shown(uri) + L"\n\nSite: " +
                                  ui_origin::shown(ui_origin::link_host(uri), 100) + L"\n\nOpen it in your browser?";
        int answer = IDNO;
#ifndef NDEBUG
        // Tests: the question goes to this file and is answered No, so no dialog shows.
        if (const auto test = util::getenv_utf8("GRAB_TEST_LINKS"); test && !test->empty()) {
            const auto before = util::read_file(util::path_from_utf8(*test)).value_or("");
            if (auto noted = util::write_file_atomic(util::path_from_utf8(*test), before + util::to_utf8(text) + "\n----\n");
                !noted) {
                OutputDebugStringW(util::to_wide(noted.error()).c_str());
            }
        } else
#endif
        {
            answer = MessageBoxW(hwnd_, text.c_str(), L"Open a link?", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        }
        asking_ = false;
        if (answer == IDYES) ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });
}

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
    const auto handler = Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
        [this](HRESULT r, ICoreWebView2Controller* c) { return on_controller(r, c); });
    // A configuration's own profile, when it has one: the colour scheme is the profile's, so two
    // configurations' windows don't set each other's (Settings → Appearance).
    ComPtr<ICoreWebView2Environment10> env10;
    ComPtr<ICoreWebView2ControllerOptions> controller_options;
    const bool own_profile = !options_.profile_name.empty() && SUCCEEDED(env_.As(&env10)) &&
                             SUCCEEDED(env10->CreateCoreWebView2ControllerOptions(&controller_options)) &&
                             controller_options != nullptr &&
                             SUCCEEDED(controller_options->put_ProfileName(options_.profile_name.c_str()));
    const HRESULT hr =
        own_profile ? env10->CreateCoreWebView2ControllerWithOptions(hwnd_, controller_options.Get(), handler.Get())
                    : env_->CreateCoreWebView2Controller(hwnd_, handler.Get());
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
    set_color_scheme(options_.color_scheme); // before the page loads: its first paint is in it

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

    // Files and folders dragged in from Explorer are uploads (UPLOADS.md 5.10): the page keeps every
    // drag from navigating the window (it takes dragover and drop everywhere) and posts a drop's
    // Files to grab, whose paths come from WebView2 below.
    ComPtr<ICoreWebView2Controller4> controller4;
    if (SUCCEEDED(controller_.As(&controller4))) controller4->put_AllowExternalDrop(TRUE);

    EventRegistrationToken token{};
    // Only grab's own page talks to grab (ui_origin.hpp).
    webview_->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                CoString source;
                if (FAILED(args->get_Source(&source.p)) || !ui_origin::is_page(source.view())) return S_OK;
                CoString json;
                if (FAILED(args->get_WebMessageAsJson(&json.p))) return S_OK;
                // The files posted beside the message (postMessageWithAdditionalObjects): WebView2
                // gives each one's path; a dropped folder comes as a File with the folder's path.
                std::vector<std::filesystem::path> paths;
                ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> args2;
                ComPtr<ICoreWebView2ObjectCollectionView> objects;
                UINT32 count = 0;
                if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&args2))) && SUCCEEDED(args2->get_AdditionalObjects(&objects)) &&
                    objects && SUCCEEDED(objects->get_Count(&count))) {
                    for (UINT32 i = 0; i < count; ++i) {
                        ComPtr<IUnknown> item;
                        ComPtr<ICoreWebView2File> file;
                        CoString path;
                        if (SUCCEEDED(objects->GetValueAtIndex(i, &item)) && item && SUCCEEDED(item.As(&file)) &&
                            SUCCEEDED(file->get_Path(&path.p)) && !path.view().empty()) {
                            paths.emplace_back(path.view());
                        }
                    }
                }
                const std::string text = util::to_utf8(json.view());
                if (on_message_) on_message_(text, std::move(paths));
                // A page opened again (restore_page) is told why once it is listening: after the
                // app's answers to its "init", which are queued to the UI thread the same way.
                if (!restored_.empty()) {
                    if (const auto msg = json::parse(text); msg && msg->string_of("type") == "init") {
                        json::Value v = json::Value::make_object();
                        v.set("type", json::Value::make_string("pageRestored"));
                        v.set("reason", json::Value::make_string(std::exchange(restored_, {})));
                        auto said = [this, out = json::stringify(v)] { post_json(out); };
                        if (options_.defer) options_.defer(std::move(said));
                        else said();
                    }
                }
                return S_OK;
            })
            .Get(),
        &token);

    // grab's page never stays away: a renderer that ended (WebView2 ends the page's process when
    // the page posts a dropped file it was never given, a file from inside a .zip: "This page is
    // having a problem", RESULT_CODE_KILLED_BAD_MESSAGE) and an error page in its place bring
    // grab's page back at once. A page that only stopped answering for a while is left alone: it
    // comes back by itself, with everything it shows.
    webview_->add_ProcessFailed(
        Callback<ICoreWebView2ProcessFailedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
                COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
                if (SUCCEEDED(args->get_ProcessFailedKind(&kind)) && kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED) {
                    restore_page("ended");
                }
                return S_OK;
            })
            .Get(),
        &token);
    webview_->add_ContentLoading(
        Callback<ICoreWebView2ContentLoadingEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2ContentLoadingEventArgs* args) -> HRESULT {
                BOOL error = FALSE;
                if (SUCCEEDED(args->get_IsErrorPage(&error)) && error) restore_page("error page");
                return S_OK;
            })
            .Get(),
        &token);

    // The window shows grab's page and nothing else; a link to a web page (from a previewed PDF)
    // opens in the browser once the user agrees.
    webview_->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                CoString uri;
                args->get_Uri(&uri.p);
                if (!ui_origin::is_page(uri.view())) {
                    args->put_Cancel(TRUE);
                    offer_link(std::wstring(uri.view()));
                }
                return S_OK;
            })
            .Get(),
        &token);
    // The viewer's frame shows previews; the PDF viewer's own frames (not http) are its business.
    webview_->add_FrameNavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                CoString uri;
                args->get_Uri(&uri.p);
                const bool web = ui_origin::is_web_link(uri.view()) || ui_origin::path_of(uri.view());
                if (web && !ui_origin::is_preview(uri.view())) {
                    args->put_Cancel(TRUE);
                    offer_link(std::wstring(uri.view()));
                }
                return S_OK;
            })
            .Get(),
        &token);
    webview_->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                CoString uri;
                args->get_Uri(&uri.p);
                args->put_Handled(TRUE); // never a second window
                offer_link(std::wstring(uri.view()));
                return S_OK;
            })
            .Get(),
        &token);

    ComPtr<ICoreWebView2_3> webview3;
    const bool dev_ui = !options_.dev_ui_dir.empty() && SUCCEEDED(webview_.As(&webview3));
    if (dev_ui) {
        // Development: serve the UI straight from the source tree so edits need only a reload.
        webview3->SetVirtualHostNameToFolderMapping(L"grab.ui", options_.dev_ui_dir.c_str(),
                                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        webview_->AddWebResourceRequestedFilter(L"https://grab.ui/preview/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    } else {
        // Normal: the UI files are resources inside the exe, served from memory.
        webview_->AddWebResourceRequestedFilter(L"https://grab.ui/*",
                                                COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    }
    {
        webview_->add_WebResourceRequested(
            Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [this, dev_ui](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                    ComPtr<ICoreWebView2WebResourceRequest> request;
                    if (FAILED(args->get_Request(&request))) return S_OK;
                    CoString uri;
                    request->get_Uri(&uri.p);
                    // A preview: answered later, from a worker thread, by its id; the deferral and
                    // the arguments wait here until the answer is back on the UI thread.
                    if (ui_origin::is_preview(uri.view()) && options_.on_preview && options_.defer) {
                        ComPtr<ICoreWebView2Deferral> deferral;
                        if (FAILED(args->GetDeferral(&deferral))) return S_OK;
                        const std::uint64_t id = ++next_request_;
                        deferred_.emplace(id, Deferred{.deferral = deferral, .args = args});
                        options_.on_preview(util::to_utf8(uri.view()), [this, id, defer = options_.defer](WebResource r) {
                            defer([this, id, r = std::move(r)] { complete(id, r); });
                        });
                        return S_OK;
                    }
                    if (dev_ui) return S_OK; // the folder mapping serves the rest
                    ComPtr<ICoreWebView2WebResourceResponse> response;
                    if (auto file = find_embedded(ui_origin::path_of(uri.view()).value_or(L""))) {
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
    const HRESULT hr = webview_->Navigate(page_url);
    if (on_ready_) {
        auto cb = std::move(on_ready_);
        on_ready_ = nullptr;
        cb(hr);
    }
    return S_OK;
}

void WebViewHost::complete(std::uint64_t id, const WebResource& r) {
    auto request = deferred_.extract(id);
    if (request.empty()) return; // closed meanwhile, or answered before
    if (env_) {
        ComPtr<IStream> stream;
        stream.Attach(new_body_stream(r.body));
        std::wstring headers = L"Content-Type: " + util::to_wide(r.content_type) +
                               L"\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff";
        if (!r.headers.empty()) headers += L"\r\n" + util::to_wide(r.headers);
        ComPtr<ICoreWebView2WebResourceResponse> response;
        if (SUCCEEDED(env_->CreateWebResourceResponse(stream.Get(), r.status, reason(r.status).c_str(), headers.c_str(),
                                                      &response))) {
            request.mapped().args->put_Response(response.Get());
        }
    }
    request.mapped().deferral->Complete();
}

void WebViewHost::restore_page(const char* why) {
    const auto now = std::chrono::steady_clock::now();
    std::erase_if(restores_, [&](auto at) { return now - at > std::chrono::minutes(1); });
    if (restores_.size() >= restore_limit || !webview_) return;
    restores_.push_back(now);
    restored_ = why;
    // After WebView2's event handler has returned, as offer_link's question.
    const auto reopen = [this] {
        if (webview_) webview_->Navigate(page_url);
    };
    if (options_.defer) options_.defer(reopen);
    else reopen();
}

void WebViewHost::resize(const RECT& bounds) {
    if (controller_) controller_->put_Bounds(bounds);
}

void WebViewHost::post_json(const std::string& json) {
    if (!webview_) return;
    CoString source; // what grab says goes to its own page only
    if (FAILED(webview_->get_Source(&source.p)) || !ui_origin::is_page(source.view())) return;
    webview_->PostWebMessageAsJson(util::to_wide(json).c_str());
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

void WebViewHost::set_color_scheme(COREWEBVIEW2_PREFERRED_COLOR_SCHEME scheme) {
    options_.color_scheme = scheme;
    ComPtr<ICoreWebView2_13> webview13;
    ComPtr<ICoreWebView2Profile> profile;
    if (webview_ == nullptr) return;
    const bool taken = SUCCEEDED(webview_.As(&webview13)) && SUCCEEDED(webview13->get_Profile(&profile)) &&
                       profile != nullptr && SUCCEEDED(profile->put_PreferredColorScheme(scheme));
    color_scheme_failed_ = !taken;
}

void WebViewHost::close() {
    deferred_.clear(); // requests still waiting: released here, on the UI thread
    if (controller_) controller_->Close();
    webview_.Reset();
    controller_.Reset();
    env_.Reset();
}

} // namespace grab::gui
