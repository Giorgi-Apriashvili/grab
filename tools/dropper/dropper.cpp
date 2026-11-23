// dropper: drags something onto a window the way a user does, for grab's drop tests (UPLOADS.md
// chunk 4). It is the drag SOURCE: a small window of its own takes a real left-button press
// (SendInput), starts an OLE drag with what was asked for, and the mouse is moved over the target
// window and let go (or the drag is cancelled, or leaves again) by SendInput too, so the target
// sees what Explorer, Edge or Outlook would hand it.
//
//   dropper (--pid N | --hwnd 0xH) --at X,Y [--via X,Y]... [--from X,Y] [--hover MS] [--still]
//           [--end drop|cancel|leave] [--leave-shown] [--raw] WHAT
//   dropper --formats WHAT          the formats WHAT offers, and nothing dragged
//
// WHAT:
//   files PATH...        real paths as CF_HDROP alone (@LIST: the paths in the UTF-8 file LIST,
//                        one a line; for shell too)
//   shell PATH...        the shell's own data object for these items, as Explorer drags them; a
//                        path inside a .zip ("C:\x\a.zip\b.png") is the zip folder's virtual file
//                        (--raw: that object itself, else one that says what the target asks of it)
//   virtual NAME[=FILE]... [--stream] [--fail-contents] [--slow-contents MS]
//                        virtual files (FileGroupDescriptorW + FileContents, no path on disk),
//                        like a mail attachment; the contents from FILE or made up; --stream
//                        hands them as IStreams (Outlook), else as memory; --fail-contents never
//                        hands them over (a .zip Explorer can't extract: WebView2 ended its page
//                        over such a drop, UPLOADS.md chunk 4), --slow-contents only after MS
//   link URL [TITLE] [--address-bar]
//                        a link as Edge drags one from a page: text, text/x-moz-url,
//                        UniformResourceLocator(W), HTML, and TITLE.url as a virtual file that
//                        Chromium is told to ignore (WebView2 sees text and a URL, no file);
//                        --address-bar: a page's address dragged from Edge's address bar (WebView2
//                        takes the .url as a dropped file too, saved as TITLE.download)
//   text TEXT            plain text alone
//   press SX,SY          someone else's drag: the window at screen pixels SX,SY (an Explorer window
//                        the caller placed there) is pressed and its own drag carried to X,Y;
//                        --end drop or leave (let go back at SX,SY)
//
// X,Y are CSS pixels of the target's page (its client area, scaled by the window's DPI); --from
// is where the drag starts (default: just left of the window, else inside its top-left corner);
// --via points it passes on the way; --hover how long it stays over X,Y (default 600 ms, moving a
// pixel every 50 ms as a hand does; --still: not moving). The target window is shown (without
// taking the focus) and kept on top during the drag, then minimized again if it was (else sent
// behind the other windows), unless --leave-shown. Mouse buttons are only ever pressed over the
// dropper's own window (or the `press` point) and let go there or over the target, never over
// another program's window. Escape is never typed: --end cancel makes the source cancel, as
// Escape would. The cursor goes back where it was.
//
// Prints what happened, one line each: what the target asked of the data (GetData, each format
// once), each change of the effect it offered (feedback), and the end: "end: drop effect=copy"
// (the target took it), "end: drop effect=none" (let go where nothing takes it), "end: cancel".
// Exit code 0 when the drag ran, 2 usage, 3 no such window, 4 the drag didn't start, 5 something
// else failed.

#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)

#include <objbase.h> // IWYU pragma: keep
#include <ole2.h>    // IWYU pragma: keep
#include <shlobj.h>  // IWYU pragma: keep
#include <shlwapi.h> // IWYU pragma: keep

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

// ---- output ---------------------------------------------------------------------------------------

std::string utf8(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

void say(const std::string& line) {
    std::fputs((line + "\n").c_str(), stdout);
    std::fflush(stdout);
}

// A target asks for the same formats again and again while the pointer moves: each answer once.
void say_once(const std::string& line) {
    static std::set<std::string> said;
    if (said.insert(line).second) say(line);
}

std::string format_name(CLIPFORMAT cf) {
    switch (cf) {
    case CF_TEXT: return "CF_TEXT";
    case CF_UNICODETEXT: return "CF_UNICODETEXT";
    case CF_HDROP: return "CF_HDROP";
    case CF_OEMTEXT: return "CF_OEMTEXT";
    case CF_LOCALE: return "CF_LOCALE";
    case CF_BITMAP: return "CF_BITMAP";
    case CF_DIB: return "CF_DIB";
    case CF_DIBV5: return "CF_DIBV5";
    default: break;
    }
    std::array<wchar_t, 256> name{};
    if (GetClipboardFormatNameW(cf, name.data(), static_cast<int>(name.size())) > 0) return utf8(name.data());
    return "format " + std::to_string(cf);
}

std::string effect_name(DWORD effect) {
    if (effect == DROPEFFECT_NONE) return "none";
    std::string s;
    if ((effect & DROPEFFECT_COPY) != 0) s += "copy";
    if ((effect & DROPEFFECT_MOVE) != 0) s += s.empty() ? "move" : "+move";
    if ((effect & DROPEFFECT_LINK) != 0) s += s.empty() ? "link" : "+link";
    return s.empty() ? std::to_string(effect) : s;
}

std::string hex(std::uint64_t v) {
    return std::format("{:#x}", v);
}

// ---- what is dragged --------------------------------------------------------------------------------

// One format the data object hands out: its bytes, as memory or a stream; FileContents once per
// file (lindex).
struct Entry {
    CLIPFORMAT cf = 0;
    DWORD tymed = TYMED_HGLOBAL;
    LONG lindex = -1;
    std::string bytes;
};

std::string bytes_of(std::wstring_view w, bool nul = true) {
    std::string s(reinterpret_cast<const char*>(w.data()), w.size() * sizeof(wchar_t));
    if (nul) s.append(sizeof(wchar_t), '\0');
    return s;
}

std::string ansi_of(std::wstring_view w) {
    const int n = WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    s.push_back('\0');
    return s;
}

// Virtual files whose contents never come (--fail-contents), or come late (--slow-contents MS).
bool contents_fail = false;
int contents_delay_ms = 0;
// Held still over the target (--still), no small moves.
bool still = false;

class DataObject final : public IDataObject {
public:
    explicit DataObject(std::vector<Entry> entries) : entries_(std::move(entries)) {}
    ~DataObject() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (out == nullptr) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *out = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (left == 0) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fe, STGMEDIUM* medium) override {
        if (fe == nullptr || medium == nullptr) return E_INVALIDARG;
        const Entry* e = find(*fe);
        const bool contents = fe->cfFormat == RegisterClipboardFormatW(CFSTR_FILECONTENTS);
        if (contents && contents_delay_ms > 0) Sleep(static_cast<DWORD>(contents_delay_ms));
        if (contents && contents_fail) {
            say_once("asked: FileContents lindex=" + std::to_string(fe->lindex) + " refused (--fail-contents)");
            return E_FAIL;
        }
        say_once("asked: " + format_name(fe->cfFormat) + " lindex=" + std::to_string(fe->lindex) + (e != nullptr ? " given" : " not here"));
        if (e == nullptr) return DV_E_FORMATETC;
        *medium = {};
        if ((e->tymed & TYMED_ISTREAM) != 0 && (fe->tymed & TYMED_ISTREAM) != 0) {
            medium->tymed = TYMED_ISTREAM;
            medium->pstm = SHCreateMemStream(reinterpret_cast<const BYTE*>(e->bytes.data()), static_cast<UINT>(e->bytes.size()));
            return medium->pstm != nullptr ? S_OK : E_OUTOFMEMORY;
        }
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, std::max<SIZE_T>(e->bytes.size(), 1));
        if (h == nullptr) return E_OUTOFMEMORY;
        if (void* p = GlobalLock(h)) {
            std::memcpy(p, e->bytes.data(), e->bytes.size());
            GlobalUnlock(h);
        }
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = h;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fe) override {
        if (fe == nullptr) return E_INVALIDARG;
        return find(*fe) != nullptr ? S_OK : DV_E_FORMATETC;
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out != nullptr) out->ptd = nullptr;
        return E_NOTIMPL;
    }
    // What a target leaves for the source (drop descriptions, the performed effect): kept, so a
    // later GetData of it answers.
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* fe, STGMEDIUM* medium, BOOL release) override {
        if (fe == nullptr || medium == nullptr) return E_INVALIDARG;
        if (medium->tymed == TYMED_HGLOBAL && medium->hGlobal != nullptr) {
            Entry e{.cf = fe->cfFormat, .tymed = TYMED_HGLOBAL, .lindex = -1, .bytes = {}};
            if (const void* p = GlobalLock(medium->hGlobal)) {
                e.bytes.assign(static_cast<const char*>(p), GlobalSize(medium->hGlobal));
                GlobalUnlock(medium->hGlobal);
            }
            std::erase_if(entries_, [&](const Entry& x) { return x.cf == e.cf; });
            entries_.push_back(std::move(e));
        }
        if (release) ReleaseStgMedium(medium);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
        if (out == nullptr) return E_POINTER;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        std::vector<FORMATETC> list;
        for (const Entry& e : entries_) {
            if (std::ranges::any_of(list, [&](const FORMATETC& f) { return f.cfFormat == e.cf; })) continue;
            list.push_back(FORMATETC{.cfFormat = e.cf, .ptd = nullptr, .dwAspect = DVASPECT_CONTENT, .lindex = -1, .tymed = e.tymed});
        }
        return SHCreateStdEnumFmtEtc(static_cast<UINT>(list.size()), list.data(), out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    const Entry* find(const FORMATETC& fe) const {
        for (const Entry& e : entries_) {
            if (e.cf != fe.cfFormat || (e.tymed & fe.tymed) == 0) continue;
            if (e.lindex != -1 && e.lindex != fe.lindex) continue;
            return &e;
        }
        return nullptr;
    }

    std::atomic<ULONG> refs_{1};
    std::vector<Entry> entries_;
};

// Another data object (the shell's), each GetData said as it passes. Interfaces other than
// IDataObject go to the inner object (the zip folder's async extraction, say).
class Logged final : public IDataObject {
public:
    explicit Logged(IDataObject* inner) : inner_(inner) { inner_->AddRef(); }
    ~Logged() { inner_->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (out == nullptr) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *out = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        return inner_->QueryInterface(riid, out);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (left == 0) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fe, STGMEDIUM* medium) override {
        const HRESULT hr = inner_->GetData(fe, medium);
        if (fe != nullptr) {
            say_once("asked: " + format_name(fe->cfFormat) + " lindex=" + std::to_string(fe->lindex) +
                     (SUCCEEDED(hr) ? " given" : " not here"));
        }
        return hr;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC* fe, STGMEDIUM* medium) override { return inner_->GetDataHere(fe, medium); }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fe) override { return inner_->QueryGetData(fe); }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC* in, FORMATETC* out) override {
        return inner_->GetCanonicalFormatEtc(in, out);
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* fe, STGMEDIUM* medium, BOOL release) override {
        return inner_->SetData(fe, medium, release);
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
        return inner_->EnumFormatEtc(direction, out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC* fe, DWORD flags, IAdviseSink* sink, DWORD* conn) override {
        return inner_->DAdvise(fe, flags, sink, conn);
    }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD conn) override { return inner_->DUnadvise(conn); }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA** out) override { return inner_->EnumDAdvise(out); }

private:
    std::atomic<ULONG> refs_{1};
    IDataObject* inner_;
};

CLIPFORMAT format(const wchar_t* name) { return static_cast<CLIPFORMAT>(RegisterClipboardFormatW(name)); }

std::vector<Entry> files_entries(const std::vector<std::wstring>& paths) {
    std::wstring list;
    for (const auto& p : paths) {
        std::array<wchar_t, MAX_PATH * 4> full{};
        const DWORD n = GetFullPathNameW(p.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
        list += n > 0 && n < full.size() ? std::wstring(full.data()) : p;
        list.push_back(L'\0');
    }
    list.push_back(L'\0');
    DROPFILES head{};
    head.pFiles = sizeof(DROPFILES);
    head.fWide = TRUE;
    std::string bytes(reinterpret_cast<const char*>(&head), sizeof head);
    bytes += bytes_of(list, false);
    return {Entry{.cf = CF_HDROP, .tymed = TYMED_HGLOBAL, .lindex = -1, .bytes = std::move(bytes)}};
}

// FileGroupDescriptorW + one FileContents per file.
void add_virtual(std::vector<Entry>& out, const std::vector<std::pair<std::wstring, std::string>>& files, DWORD tymed) {
    std::string group(sizeof(UINT), '\0');
    const UINT count = static_cast<UINT>(files.size());
    std::memcpy(group.data(), &count, sizeof count);
    LONG index = 0;
    for (const auto& [name, content] : files) {
        FILEDESCRIPTORW d{};
        d.dwFlags = static_cast<DWORD>(FD_FILESIZE | FD_WRITESTIME | FD_UNICODE | FD_PROGRESSUI); // FD_UNICODE is 0x80000000
        d.nFileSizeLow = static_cast<DWORD>(content.size());
        d.nFileSizeHigh = static_cast<DWORD>(static_cast<std::uint64_t>(content.size()) >> 32);
        GetSystemTimeAsFileTime(&d.ftLastWriteTime);
        wcsncpy_s(d.cFileName, name.c_str(), _TRUNCATE);
        group.append(reinterpret_cast<const char*>(&d), sizeof d);
        out.push_back(Entry{.cf = format(CFSTR_FILECONTENTS), .tymed = tymed, .lindex = index++, .bytes = content});
    }
    out.insert(out.begin(), Entry{.cf = format(CFSTR_FILEDESCRIPTORW), .tymed = TYMED_HGLOBAL, .lindex = -1, .bytes = std::move(group)});
}

std::optional<std::string> read_all(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

std::vector<Entry> text_entries(const std::wstring& text) {
    return {Entry{.cf = CF_UNICODETEXT, .tymed = TYMED_HGLOBAL, .lindex = -1, .bytes = bytes_of(text)},
            Entry{.cf = CF_TEXT, .tymed = TYMED_HGLOBAL, .lindex = -1, .bytes = ansi_of(text)}};
}

// A link as Edge 158 drags one (its formats in its order, read by a drop target of our own,
// UPLOADS.md chunk 4 fix): from a page, the page's origin as chromium/x-renderer-taint, text,
// text/x-moz-url, TITLE.url as a virtual file marked chromium/x-ignore-file-contents,
// UniformResourceLocator(W) and the link as HTML; WebView2 then shows the page text/plain,
// text/uri-list and text/html, and no file. From the address bar (`address_bar`, a page's own
// address): no taint, text or HTML, and WebView2 takes the shortcut as a dropped file (saved as
// TITLE.download in its temp folder).
std::vector<Entry> link_entries(const std::wstring& url, const std::wstring& title, bool address_bar) {
    std::vector<Entry> out;
    const auto text = text_entries(url);
    if (!address_bar) {
        out.push_back(Entry{.cf = format(L"chromium/x-renderer-taint"), .tymed = TYMED_HGLOBAL, .lindex = -1, .bytes = std::string("file://") + '\0'});
        out.insert(out.end(), text.begin(), text.end());
    }
    out.push_back(Entry{.cf = format(L"text/x-moz-url"), .tymed = TYMED_HGLOBAL, .lindex = -1, .bytes = bytes_of(url + L"\n" + title)});
    std::vector<Entry> shortcut;
    add_virtual(shortcut, {{title + L".url", "[InternetShortcut]\r\nURL=" + utf8(url) + "\r\n"}}, TYMED_HGLOBAL);
    out.insert(out.end(), shortcut.begin(), shortcut.end());
    out.push_back(Entry{.cf = format(L"chromium/x-ignore-file-contents"), .tymed = TYMED_HGLOBAL, .lindex = -1, .bytes = std::string(1, '\0')});
    out.push_back(Entry{.cf = format(CFSTR_INETURLW), .tymed = TYMED_HGLOBAL, .lindex = -1, .bytes = bytes_of(url)});
    out.push_back(Entry{.cf = format(CFSTR_INETURLA), .tymed = TYMED_HGLOBAL, .lindex = -1, .bytes = ansi_of(url)});
    if (address_bar) {
        out.insert(out.end(), text.begin(), text.end());
        return out;
    }
    // CF_HTML: the header's offsets count bytes from its start (UTF-8).
    const std::string a = "<a href=\"" + utf8(url) + "\">" + utf8(title) + "</a>";
    const std::string head = "Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\nStartFragment:0000000000\r\nEndFragment:0000000000\r\nSourceURL:" +
                             utf8(url) + "\r\n";
    const std::string before = "<html>\r\n<body>\r\n<!--StartFragment-->";
    const std::string html_text = before + a + "<!--EndFragment-->\r\n</body>\r\n</html>";
    std::string html = head + html_text;
    const auto put = [&](std::string_view key, std::size_t value) {
        html.replace(html.find(key) + key.size(), 10, std::format("{:010}", value));
    };
    put("StartHTML:", head.size());
    put("EndHTML:", html.size());
    put("StartFragment:", head.size() + before.size());
    put("EndFragment:", head.size() + before.size() + a.size());
    out.push_back(Entry{.cf = format(L"HTML Format"), .tymed = TYMED_HGLOBAL, .lindex = -1, .bytes = html});
    out.push_back(Entry{.cf = format(L"text/html"), .tymed = TYMED_HGLOBAL, .lindex = -1, .bytes = a});
    return out;
}

// The shell's data object for `paths`, as Explorer makes it for a drag of those items.
IDataObject* shell_object(const std::vector<std::wstring>& paths) {
    std::vector<PIDLIST_ABSOLUTE> ids;
    for (const auto& p : paths) {
        PIDLIST_ABSOLUTE id = nullptr;
        const HRESULT hr = SHParseDisplayName(p.c_str(), nullptr, &id, 0, nullptr);
        if (FAILED(hr)) {
            say("error: the shell doesn't know " + utf8(p) + " (" + hex(static_cast<std::uint32_t>(hr)) + ")");
            for (auto* i : ids) CoTaskMemFree(i);
            return nullptr;
        }
        ids.push_back(id);
    }
    IShellItemArray* items = nullptr;
    HRESULT hr = SHCreateShellItemArrayFromIDLists(static_cast<UINT>(ids.size()), const_cast<PCIDLIST_ABSOLUTE_ARRAY>(ids.data()), &items);
    for (auto* i : ids) CoTaskMemFree(i);
    IDataObject* data = nullptr;
    if (SUCCEEDED(hr)) {
        hr = items->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
        items->Release();
    }
    if (FAILED(hr)) say("error: no data object from the shell (" + hex(static_cast<std::uint32_t>(hr)) + ")");
    return data;
}

// ---- the drag ---------------------------------------------------------------------------------------

std::atomic<bool> started{false};
std::atomic<bool> cancel{false};
std::atomic<bool> ended{false};
DWORD last_effect = 0xffffffff;

class DropSource final : public IDropSource {
public:
    ~DropSource() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (out == nullptr) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropSource) {
            *out = static_cast<IDropSource*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (left == 0) delete this;
        return left;
    }
    HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override {
        if (escape || cancel) return DRAGDROP_S_CANCEL;
        if ((keys & MK_LBUTTON) == 0) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect) override {
        if (effect != last_effect) {
            last_effect = effect;
            say("feedback: " + effect_name(effect));
        }
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }

private:
    std::atomic<ULONG> refs_{1};
};

IDataObject* dragged = nullptr;
HRESULT drag_result = E_FAIL;
DWORD drag_effect = 0;

LRESULT CALLBACK source_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_LBUTTONDOWN:
        if (!started && dragged != nullptr) {
            auto* source = new DropSource();
            DWORD effect = 0;
            started = true; // OLE's loop asks the source only as input comes
            drag_result = DoDragDrop(dragged, source, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
            drag_effect = effect;
            source->Release();
            ended = true;
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT r;
        GetClientRect(hwnd, &r);
        FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH)));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ---- the mouse --------------------------------------------------------------------------------------

void send_mouse(DWORD flags, POINT at) {
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = std::max(GetSystemMetrics(SM_CXVIRTUALSCREEN), 2);
    const int vh = std::max(GetSystemMetrics(SM_CYVIRTUALSCREEN), 2);
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = MulDiv(at.x - vx, 65535, vw - 1);
    in.mi.dy = MulDiv(at.y - vy, 65535, vh - 1);
    in.mi.dwFlags = flags | MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    SendInput(1, &in, sizeof in);
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// From `from` to `to` in steps, as a hand moves.
void glide(POINT from, POINT to, int steps = 16, int gap_ms = 15) {
    for (int i = 1; i <= steps; ++i) {
        send_mouse(0, POINT{.x = from.x + (to.x - from.x) * i / steps, .y = from.y + (to.y - from.y) * i / steps});
        sleep_ms(gap_ms);
    }
}

bool wait_for(const std::atomic<bool>& flag, int ms) {
    for (int waited = 0; !flag && waited < ms; waited += 10) sleep_ms(10);
    return flag;
}

// ---- the target window ------------------------------------------------------------------------------

struct Find {
    DWORD pid = 0;
    HWND found = nullptr;
};

BOOL CALLBACK find_window(HWND hwnd, LPARAM lp) {
    auto* f = reinterpret_cast<Find*>(lp);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != f->pid || GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
    std::array<wchar_t, 64> cls{};
    GetClassNameW(hwnd, cls.data(), static_cast<int>(cls.size()));
    if (std::wstring_view(cls.data()).starts_with(L"grab")) {
        f->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

std::optional<POINT> parse_point(const std::wstring& s) {
    const auto comma = s.find(L',');
    if (comma == std::wstring::npos) return std::nullopt;
    try {
        return POINT{.x = std::stoi(s.substr(0, comma)), .y = std::stoi(s.substr(comma + 1))};
    } catch (...) {
        return std::nullopt;
    }
}

int usage() {
    say("usage: dropper (--pid N | --hwnd 0xH) --at X,Y [--via X,Y]... [--from X,Y] [--hover MS] [--still] "
        "[--end drop|cancel|leave] [--leave-shown] [--raw] (files PATH... | shell PATH... | virtual NAME[=FILE]... "
        "[--stream] [--fail-contents] [--slow-contents MS] | link URL [TITLE] [--address-bar] | text TEXT | press SX,SY)");
    say("       dropper --formats WHAT   (see the top of tools/dropper/dropper.cpp)");
    return 2;
}

int drag_from_own_window(POINT start, POINT drop_at, const std::vector<POINT>& via, const std::function<POINT(POINT)>& screen,
                         double scale, int hover, const std::wstring& end, POINT cursor_was) {
    // The dropper's own window, where the drag starts.
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = source_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"grab-dropper-source";
    RegisterClassExW(&wc);
    const int side = static_cast<int>(36 * scale);
    HWND source = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"dropper", WS_POPUP,
                                  start.x - side / 2, start.y - side / 2, side, side, nullptr, nullptr, wc.hInstance, nullptr);
    if (source == nullptr) return 5;
    ShowWindow(source, SW_SHOWNOACTIVATE);
    UpdateWindow(source);

    int code = 0;
    std::thread hand([&] {
        // Pressed until the drag starts: a press can be lost (the user's own mouse, a window that
        // was on top for a moment); nothing has reached the target before the drag starts. Only
        // ever over the dropper's own window: another window there now, and it waits.
        for (int tries = 0; tries < 5 && !started; ++tries) {
            if (tries > 0) {
                say("retry: the drag didn't start, pressing again");
                SetWindowPos(source, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                sleep_ms(500);
            }
            sleep_ms(150);
            send_mouse(0, start);
            sleep_ms(80);
            if (WindowFromPoint(start) != source) {
                say("retry: another window is where the drag starts");
                continue;
            }
            send_mouse(MOUSEEVENTF_LEFTDOWN, start);
            if (!wait_for(started, 3000)) send_mouse(MOUSEEVENTF_LEFTUP, start);
        }
        if (!started) {
            say("error: the drag didn't start"); // each press was let go above
            code = 4;
        } else {
            POINT was = start;
            for (const POINT& v : via) {
                const POINT p = screen(v);
                glide(was, p);
                was = p;
            }
            glide(was, drop_at);
            // Over the target: small moves keep dragover coming, as a hand never holds still
            // (--still: none).
            for (int t = 0; t < hover; t += 50) {
                if (!still) send_mouse(0, POINT{.x = drop_at.x + (t / 50) % 2, .y = drop_at.y});
                sleep_ms(50);
            }
            send_mouse(0, drop_at);
            sleep_ms(30);
            if (end == L"drop") {
                send_mouse(MOUSEEVENTF_LEFTUP, drop_at);
            } else if (end == L"cancel") {
                cancel = true; // as Escape would, from the source
                wait_for(ended, 5000);
                send_mouse(0, start);
                sleep_ms(30);
                send_mouse(MOUSEEVENTF_LEFTUP, start);
            } else { // leave: back out to the dropper's window, let go there
                glide(drop_at, start);
                sleep_ms(200);
                send_mouse(MOUSEEVENTF_LEFTUP, start);
            }
            if (!wait_for(ended, 30000)) {
                say("error: the drag didn't end within 30 s");
                code = 5;
            }
        }
        SetCursorPos(cursor_was.x, cursor_was.y);
        PostMessageW(source, WM_CLOSE, 0, 0);
    });

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    hand.join();

    if (ended) {
        const char* how = drag_result == DRAGDROP_S_DROP ? "drop" : drag_result == DRAGDROP_S_CANCEL ? "cancel" : "error";
        say(std::string("end: ") + how + " effect=" + effect_name(drag_effect) + " hr=" + hex(static_cast<std::uint32_t>(drag_result)));
    }
    return code;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(OleInitialize(nullptr))) return 5;

    DWORD pid = 0;
    HWND target = nullptr;
    std::optional<POINT> at;
    std::optional<POINT> from;
    std::vector<POINT> via;
    int hover = 600;
    std::wstring end = L"drop";
    bool leave_shown = false;
    bool raw = false;
    bool formats = false;
    bool stream = false;
    bool address_bar = false;
    std::wstring what;
    std::vector<std::wstring> args;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool more = i + 1 < argc;
        if (what.empty() && a == L"--pid" && more) pid = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
        else if (what.empty() && a == L"--hwnd" && more) target = reinterpret_cast<HWND>(static_cast<std::uintptr_t>(std::wcstoull(argv[++i], nullptr, 16)));
        else if (what.empty() && a == L"--at" && more) at = parse_point(argv[++i]);
        else if (what.empty() && a == L"--from" && more) from = parse_point(argv[++i]);
        else if (what.empty() && a == L"--via" && more) {
            if (auto p = parse_point(argv[++i])) via.push_back(*p);
            else return usage();
        } else if (what.empty() && a == L"--hover" && more) hover = std::max(0, _wtoi(argv[++i]));
        else if (what.empty() && a == L"--end" && more) end = argv[++i];
        else if (what.empty() && a == L"--leave-shown") leave_shown = true;
        else if (what.empty() && a == L"--raw") raw = true;
        else if (what.empty() && a == L"--still") still = true;
        else if (what.empty() && a == L"--formats") formats = true;
        else if (a == L"--stream") stream = true;
        else if (a == L"--address-bar") address_bar = true;
        else if (a == L"--fail-contents") contents_fail = true;
        else if (a == L"--slow-contents" && more) contents_delay_ms = std::max(0, _wtoi(argv[++i]));
        else if (what.empty()) what = a;
        else args.push_back(a);
    }
    if (what.empty() || (end != L"drop" && end != L"cancel" && end != L"leave")) return usage();
    // @LIST: the paths in the UTF-8 file LIST, one a line (more than a command line holds).
    if (what == L"files" || what == L"shell") {
        std::vector<std::wstring> paths;
        for (const auto& a : args) {
            if (!a.starts_with(L'@')) {
                paths.push_back(a);
                continue;
            }
            const auto text = read_all(a.substr(1));
            if (!text) {
                say("error: can't read " + utf8(a.substr(1)));
                return 5;
            }
            std::istringstream lines(*text);
            for (std::string line; std::getline(lines, line);) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (line.empty()) continue;
                const int n = MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0);
                std::wstring w(static_cast<std::size_t>(n), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), w.data(), n);
                paths.push_back(std::move(w));
            }
        }
        args = std::move(paths);
    }

    // What is dragged.
    if (what == L"files" && !args.empty()) {
        dragged = new DataObject(files_entries(args));
    } else if (what == L"shell" && !args.empty()) {
        IDataObject* shell = shell_object(args);
        if (shell == nullptr) return 5;
        if (raw) {
            dragged = shell;
        } else {
            dragged = new Logged(shell);
            shell->Release();
        }
    } else if (what == L"virtual" && !args.empty()) {
        std::vector<std::pair<std::wstring, std::string>> files;
        for (const auto& a : args) {
            const auto eq = a.find(L'=');
            const std::wstring name = a.substr(0, eq);
            std::string content;
            if (eq != std::wstring::npos) {
                auto text = read_all(a.substr(eq + 1));
                if (!text) {
                    say("error: can't read " + utf8(a.substr(eq + 1)));
                    return 5;
                }
                content = std::move(*text);
            } else {
                for (int k = 0; k < 64; ++k) content += "a virtual file, no path on disk: " + utf8(name) + "\n";
            }
            files.emplace_back(name, std::move(content));
        }
        std::vector<Entry> entries;
        add_virtual(entries, files, stream ? TYMED_ISTREAM : TYMED_HGLOBAL);
        dragged = new DataObject(std::move(entries));
    } else if (what == L"link" && !args.empty()) {
        dragged = new DataObject(link_entries(args[0], args.size() > 1 ? args[1] : L"Example Domain", address_bar));
    } else if (what == L"text" && !args.empty()) {
        dragged = new DataObject(text_entries(args[0]));
    } else if (what == L"press" && args.size() == 1 && parse_point(args[0]) && !formats && end != L"cancel") {
        // Someone else's drag (Explorer's): pressed there, the dropper only moves the mouse.
    } else {
        return usage();
    }

    if (formats) {
        IEnumFORMATETC* list = nullptr;
        if (SUCCEEDED(dragged->EnumFormatEtc(DATADIR_GET, &list)) && list != nullptr) {
            FORMATETC fe{};
            while (list->Next(1, &fe, nullptr) == S_OK) {
                say("format: " + format_name(fe.cfFormat) + " tymed=" + std::to_string(fe.tymed) + " lindex=" + std::to_string(fe.lindex));
                if (fe.ptd != nullptr) CoTaskMemFree(fe.ptd);
            }
            list->Release();
        }
        dragged->Release();
        OleUninitialize();
        return 0;
    }

    if (target == nullptr && pid != 0) {
        Find f{.pid = pid, .found = nullptr};
        EnumWindows(find_window, reinterpret_cast<LPARAM>(&f));
        target = f.found;
    }
    if (target == nullptr || !IsWindow(target)) {
        say("error: no window to drop on");
        return 3;
    }
    if (!at) return usage();

    // The target shown and on top for the drag, without the focus.
    const bool was_minimized = IsIconic(target) != FALSE;
    if (was_minimized) ShowWindow(target, SW_SHOWNOACTIVATE);
    SetWindowPos(target, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (was_minimized) sleep_ms(700); // a WebView2 that was minimized lays its page out again
    const double scale = GetDpiForWindow(target) / 96.0;
    POINT origin{.x = 0, .y = 0};
    ClientToScreen(target, &origin);
    RECT client{};
    GetClientRect(target, &client);
    auto screen = [&](POINT css) {
        return POINT{.x = origin.x + std::lround(css.x * scale), .y = origin.y + std::lround(css.y * scale)};
    };
    const POINT drop_at = screen(*at);
    // Where the drag starts: --from, else just left of the window when that is on a screen, else
    // inside its top-left corner.
    POINT start{};
    if (from) {
        start = screen(*from);
    } else {
        start = POINT{.x = origin.x - 40, .y = drop_at.y};
        if (MonitorFromPoint(start, MONITOR_DEFAULTTONULL) == nullptr) start = POINT{.x = origin.x + 24, .y = origin.y + 24};
    }
    const bool press = what == L"press";
    if (press) start = *parse_point(args[0]); // screen pixels
    say("target: " + hex(reinterpret_cast<std::uintptr_t>(target)) + " scale=" + std::to_string(scale) + " drop at " +
        std::to_string(drop_at.x) + "," + std::to_string(drop_at.y) + " from " + std::to_string(start.x) + "," + std::to_string(start.y));

    POINT cursor_was{};
    GetCursorPos(&cursor_was);
    if (press) {
        // The window there starts its own drag once the pointer moves past the drag threshold.
        sleep_ms(150);
        send_mouse(0, start);
        sleep_ms(120);
        send_mouse(MOUSEEVENTF_LEFTDOWN, start);
        sleep_ms(120);
        glide(start, POINT{.x = start.x + 24, .y = start.y + 8}, 8, 20);
        sleep_ms(300);
        glide(POINT{.x = start.x + 24, .y = start.y + 8}, drop_at);
        for (int t = 0; t < hover; t += 50) {
            send_mouse(0, POINT{.x = drop_at.x + (t / 50) % 2, .y = drop_at.y});
            sleep_ms(50);
        }
        if (end == L"leave") {
            glide(drop_at, start);
            sleep_ms(200);
            send_mouse(MOUSEEVENTF_LEFTUP, start);
        } else {
            send_mouse(MOUSEEVENTF_LEFTUP, drop_at);
        }
        sleep_ms(2000);
        SetCursorPos(cursor_was.x, cursor_was.y);
        say("end: let go (the drag was the other window's)");
    }

    int code = 0;
    if (!press) code = drag_from_own_window(start, drop_at, via, screen, scale, hover, end, cursor_was);
    // The target as it was: minimized again, or behind the other windows.
    SetWindowPos(target, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (!leave_shown) {
        if (was_minimized) ShowWindow(target, SW_SHOWMINNOACTIVE);
        else SetWindowPos(target, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    if (dragged != nullptr) dragged->Release();
    OleUninitialize();
    return code;
}

