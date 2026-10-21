#include "ui_origin.hpp"

#include <doctest/doctest.h>

#include <string>

using namespace grab;

TEST_CASE("only grab's own page is the window's page (G40)") {
    CHECK(ui_origin::is_page(L"https://grab.ui/index.html"));
    CHECK(ui_origin::is_page(L"https://grab.ui/index.html#downloads"));
    // Look-alikes that a bare "starts with https://grab.ui" let through.
    CHECK_FALSE(ui_origin::is_page(L"https://grab.ui@example.com/"));
    CHECK_FALSE(ui_origin::is_page(L"https://grab.ui.example.com/index.html"));
    CHECK_FALSE(ui_origin::is_page(L"https://grab.uix.com/"));
    CHECK_FALSE(ui_origin::is_page(L"https://grab.ui:8443/index.html"));
    // grab's own, but not the page: a preview a PDF link points at replaced the whole window.
    CHECK_FALSE(ui_origin::is_page(L"https://grab.ui/preview/Main/x.txt"));
    CHECK_FALSE(ui_origin::is_page(L"https://grab.ui/index.html?x"));
    CHECK_FALSE(ui_origin::is_page(L"https://grab.ui/index.htmlx"));
    CHECK_FALSE(ui_origin::is_page(L"https://grab.ui"));
    CHECK_FALSE(ui_origin::is_page(L"http://grab.ui/index.html"));
}

TEST_CASE("grab's own resources, previews and links elsewhere") {
    CHECK(ui_origin::path_of(L"https://grab.ui/app.js?v=1") == L"/app.js");
    CHECK(ui_origin::path_of(L"https://grab.ui/") == L"/");
    CHECK_FALSE(ui_origin::path_of(L"https://grab.ui").has_value());
    CHECK_FALSE(ui_origin::path_of(L"https://grab.ui.example.com/app.js").has_value());
    CHECK_FALSE(ui_origin::path_of(L"https://grab.ui@example.com/app.js").has_value());

    CHECK(ui_origin::is_preview(L"https://grab.ui/preview/Main/a.pdf"));
    CHECK_FALSE(ui_origin::is_preview(L"https://grab.ui/previewx"));
    CHECK_FALSE(ui_origin::is_preview(L"https://grab.ui.example.com/preview/Main/a.pdf"));

    CHECK(ui_origin::is_web_link(L"https://example.com/"));
    CHECK(ui_origin::is_web_link(L"HTTP://example.com/"));
    CHECK(ui_origin::is_web_link(L"https://grab.ui@example.com/"));
    CHECK_FALSE(ui_origin::is_web_link(L"https://grab.ui/preview/Main/x.txt")); // grab's own: never the browser
    CHECK_FALSE(ui_origin::is_web_link(L"file:///C:/Windows/System32/calc.exe"));
    CHECK_FALSE(ui_origin::is_web_link(L"javascript:alert(1)"));
    CHECK_FALSE(ui_origin::is_web_link(L"ms-settings:"));
}

TEST_CASE("the question shows where a link really goes") {
    CHECK(ui_origin::link_host(L"https://grab.ui@example.com/x") == L"example.com");
    CHECK(ui_origin::link_host(L"https://a:b@example.com:8443/x?y") == L"example.com");
    CHECK(ui_origin::link_host(L"https://grab.ui.example.com") == L"grab.ui.example.com");
    CHECK(ui_origin::link_host(L"http://[2001:db8::1]:8080/") == L"[2001:db8::1]");
    CHECK(ui_origin::link_host(L"https://example.com\\@evil.example/") == L"example.com");
    CHECK(ui_origin::shown(L"https://a.example/\u202Efdp.exe") == L"https://a.example/\uFFFDfdp.exe");
    CHECK(ui_origin::shown(L"https://a.example/x\ny") == L"https://a.example/x\uFFFDy");
    CHECK(ui_origin::shown(std::wstring(400, L'a'), 300) == std::wstring(300, L'a') + L"\u2026");
}
