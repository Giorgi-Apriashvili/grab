#pragma once

// A read-only COM IStream over a body held once and shared: the window's preview answers
// (webview.cpp), so a preview of up to 128 MiB is no longer held three times (the preview cache's
// copy, the answer, and SHCreateMemStream's copy of it). WebView2 reads a response's stream on the
// UI thread, 32 KiB per Read, so a Read never waits for anything but its own short lock; each
// stream has its own position, and Clone shares the body.

#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)

// WIN32_LEAN_AND_MEAN leaves COM out of windows.h.
#include <objbase.h> // IWYU pragma: keep

#include <memory>
#include <string>

namespace grab {

// A new stream at position 0 over `body` (empty when null), with one reference for the caller.
[[nodiscard]] IStream* new_body_stream(std::shared_ptr<const std::string> body);

} // namespace grab
