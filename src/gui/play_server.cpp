#include "play_server.hpp"

#include "preview.hpp"
#include "process.hpp"
#include "util.hpp"

#include <winhttp.h>
#include <ws2tcpip.h> // IWYU pragma: keep (the socket API)

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <format>
#include <functional>
#include <random>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace grab::gui {

namespace {

constexpr int pool_ids = 1'000'000;   // in the servers' pools, apart from the downloads' ids
constexpr int warm_reads = 6;         // connections at most, from the server's pool
constexpr int warm_weight = 4;        // High priority while it warms up
constexpr std::uint64_t min_split = std::uint64_t{2} << 20; // a long read is worth its login
constexpr std::uint64_t plan_after = 64 * 1024;             // the first bytes that tell how far the start goes
constexpr auto ready_within = std::chrono::seconds(60);
constexpr auto idle_after = std::chrono::minutes(5);
constexpr std::size_t kept = 2;       // files kept warm at most
constexpr std::size_t send_piece = std::size_t{1} << 20;

std::string random_token() {
    std::random_device rd;
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (int i = 0; i < 4; ++i) {
        auto v = rd();
        for (int k = 0; k < 8; ++k) {
            out += hex[v & 0xF];
            v >>= 4;
        }
    }
    return out;
}

// What a player is told the file is (it looks at the bytes anyway).
std::string media_type(std::string_view name) {
    const auto dot = name.rfind('.');
    const std::string ext = dot == std::string_view::npos ? "" : util::to_lower(name.substr(dot + 1));
    if (ext == "mkv") return "video/x-matroska";
    if (ext == "mka") return "audio/x-matroska";
    if (ext == "mp4" || ext == "m4v") return "video/mp4";
    if (ext == "webm") return "video/webm";
    if (ext == "avi") return "video/x-msvideo";
    if (ext == "mp3") return "audio/mpeg";
    if (ext == "flac") return "audio/flac";
    if (ext == "m4a" || ext == "m4b") return "audio/mp4";
    return "application/octet-stream";
}

bool send_all(SOCKET s, std::string_view data) {
    while (!data.empty()) {
        const int n = ::send(s, data.data(), static_cast<int>(std::min<std::size_t>(data.size(), send_piece)), 0);
        if (n <= 0) return false;
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

struct Internet {
    HINTERNET h = nullptr;
    explicit Internet(HINTERNET handle) : h(handle) {}
    ~Internet() {
        if (h != nullptr) WinHttpCloseHandle(h);
    }
    Internet(const Internet&) = delete;
    Internet& operator=(const Internet&) = delete;
};

// A request to rclone serve ("http://127.0.0.1:PORT/..."), sent; nullopt when it couldn't be.
struct Upstream {
    std::unique_ptr<Internet> session, connection, request;
    DWORD status = 0;
};

std::optional<Upstream> ask_upstream(const std::string& url, const wchar_t* method, std::wstring_view headers) {
    constexpr std::string_view local = "http://127.0.0.1:";
    if (!url.starts_with(local)) return std::nullopt;
    const std::string_view rest = std::string_view(url).substr(local.size());
    const auto slash = rest.find('/');
    unsigned port = 0;
    const auto [end, err] = std::from_chars(rest.data(), rest.data() + std::min(slash, rest.size()), port);
    if (err != std::errc{} || slash == std::string_view::npos || port == 0 || port > 65535) return std::nullopt;
    const std::wstring path = util::to_wide(rest.substr(slash));
    Upstream u;
    u.session = std::make_unique<Internet>(
        WinHttpOpen(L"grab", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (u.session->h == nullptr) return std::nullopt;
    WinHttpSetTimeouts(u.session->h, 5000, 5000, 30000, 30000);
    u.connection = std::make_unique<Internet>(WinHttpConnect(u.session->h, L"127.0.0.1", static_cast<INTERNET_PORT>(port), 0));
    if (u.connection->h == nullptr) return std::nullopt;
    u.request = std::make_unique<Internet>(WinHttpOpenRequest(u.connection->h, method, path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                              WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_ESCAPE_DISABLE));
    if (u.request->h == nullptr) return std::nullopt;
    const std::wstring h(headers);
    if (!WinHttpSendRequest(u.request->h, h.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : h.c_str(),
                            h.empty() ? 0 : static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(u.request->h, nullptr)) {
        return std::nullopt;
    }
    DWORD size = sizeof(u.status);
    WinHttpQueryHeaders(u.request->h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &u.status, &size, WINHTTP_NO_HEADER_INDEX);
    return u;
}

std::string header_of(HINTERNET request, DWORD which) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, which, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (size == 0) return {};
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, which, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size, WINHTTP_NO_HEADER_INDEX)) return {};
    value.resize(size / sizeof(wchar_t));
    return util::to_utf8(value);
}

} // namespace

struct PlayServer::Stream {
    int id = 0;
    File file;
    std::uint64_t size = 0;
    std::string type;
    std::uint64_t tail_at = 0; // the end's read starts here (== size: none)
    std::mutex m;
    std::condition_variable_any cv;
    std::vector<Segment> segments;   // only ever appended to: an index stays the same segment
    std::vector<std::string> data;   // each segment's bytes so far, from its start
    bool planned = false;            // `head` is decided, from the first bytes
    std::uint64_t head = 0;          // what the player needs of the start: [0, head)
    std::chrono::steady_clock::time_point used;
    Progress on_progress;            // until what the player needs is in
    int working = 0;                 // fetching threads still running
    std::stop_source stop;
    std::vector<std::jthread> workers; // last: joined first, after the stop

    ~Stream() { stop.request_stop(); }

    std::optional<std::size_t> holding(std::uint64_t pos) const {
        for (std::size_t i = 0; i < segments.size(); ++i) {
            if (pos >= segments[i].start && pos < segments[i].start + segments[i].length) return i;
        }
        return std::nullopt;
    }
    std::uint64_t next_after(std::uint64_t pos, std::uint64_t limit) const {
        for (const auto& g : segments) {
            if (g.start > pos) limit = std::min(limit, g.start);
        }
        return limit;
    }
    void add(std::uint64_t start, std::uint64_t end) {
        segments.push_back(Segment{start, end - start});
        data.emplace_back();
    }
    bool ended(std::size_t i) const {
        return segments[i].state == Segment::State::done || segments[i].state == Segment::State::failed;
    }
    // Once the first bytes are in: how much of the start the player needs, and the long reads
    // that fetch the rest of it, side by side.
    void plan(std::string_view first) {
        head = preview::warm_start(first, size);
        std::uint64_t from = 0; // where the reads of the start end so far
        for (const auto& g : segments) {
            if (g.start < tail_at) from = std::max(from, g.start + g.length);
        }
        const std::uint64_t until = std::min(head, tail_at);
        if (until > from) {
            const std::uint64_t span = until - from;
            const auto parts = std::clamp<std::uint64_t>(span / min_split, 1, warm_reads - 2);
            const std::uint64_t each = (span + parts - 1) / parts;
            for (std::uint64_t at = from; at < until; at += each) add(at, std::min(until, at + each));
        }
        planned = true;
    }
    // What the player needs, and how much of it is in: [0, head) and the end.
    std::pair<std::uint64_t, std::uint64_t> needed() const {
        std::uint64_t done = 0;
        std::uint64_t total = 0;
        const std::uint64_t want = planned ? head : (segments.empty() ? 0 : segments[0].length);
        for (std::size_t i = 0; i < segments.size(); ++i) {
            const auto& g = segments[i];
            std::uint64_t part = 0;
            if (g.start >= tail_at) part = g.length;
            else if (g.start < want) part = std::min(g.length, want - g.start);
            total += part;
            done += g.state == Segment::State::failed ? part : std::min<std::uint64_t>(part, data[i].size());
        }
        return {done, total};
    }
    bool ready() const {
        if (!planned) return false;
        const auto [done, total] = needed();
        return done >= total;
    }
};

PlayServer::PlayServer(fetch::Connections& connections) : connections_(connections) {}

PlayServer::~PlayServer() {
    closing_ = true;
    std::unique_lock lock(mutex_);
    for (auto& [id, s] : streams_) s->stop.request_stop();
    if (listener_ != INVALID_SOCKET) {
        closesocket(listener_);
        listener_ = INVALID_SOCKET;
    }
    for (const SOCKET c : clients_) shutdown(c, SD_BOTH); // their threads end at the next send or wait
    clients_done_.wait(lock, [this] { return clients_.empty(); });
    lock.unlock();
    if (acceptor_.joinable()) acceptor_.join();
    std::map<int, std::shared_ptr<Stream>> streams;
    {
        std::lock_guard again(mutex_);
        streams.swap(streams_);
    }
    streams.clear(); // joins the fetching threads
    if (!base_.empty()) WSACleanup();
}

std::expected<void, std::string> PlayServer::listen() {
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return util::fail("cannot use the network (WSAStartup)");
    const SOCKET l = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in at{};
    at.sin_family = AF_INET;
    at.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // this PC only
    at.sin_port = 0;
    int length = sizeof(at);
    if (l == INVALID_SOCKET || ::bind(l, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) != 0 ||
        ::listen(l, SOMAXCONN) != 0 || ::getsockname(l, reinterpret_cast<sockaddr*>(&at), &length) != 0) {
        const int error = WSAGetLastError();
        if (l != INVALID_SOCKET) closesocket(l);
        WSACleanup();
        return util::failf("cannot start the player's stream: {}", std::system_category().message(error));
    }
    listener_ = l;
    token_ = random_token();
    base_ = std::format("http://127.0.0.1:{}/{}/", ntohs(at.sin_port), token_);
    acceptor_ = std::jthread([this] { accept_loop(); });
    return {};
}

void PlayServer::accept_loop() {
    for (;;) {
        SOCKET listener = INVALID_SOCKET;
        {
            std::lock_guard lock(mutex_);
            listener = listener_;
        }
        if (listener == INVALID_SOCKET || closing_) return;
        const SOCKET c = ::accept(listener, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            if (closing_) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        {
            std::lock_guard lock(mutex_);
            if (closing_) {
                closesocket(c);
                return;
            }
            clients_.insert(c);
        }
        std::thread([this, c] {
            serve(c);
            std::lock_guard lock(mutex_);
            clients_.erase(c);
            closesocket(c);
            clients_done_.notify_all();
        }).detach();
    }
}

void PlayServer::forget_old() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::pair<std::chrono::steady_clock::time_point, int>> by_use;
    for (auto& [id, s] : streams_) {
        std::lock_guard lock(s->m);
        by_use.emplace_back(s->used, id);
    }
    std::ranges::sort(by_use, std::greater<>{});
    for (std::size_t i = 0; i < by_use.size(); ++i) {
        // The newest `kept - 1` stay (the one about to be added makes `kept`), unless long idle.
        if (i + 1 < kept && now - by_use[i].first < idle_after) continue;
        const auto it = streams_.find(by_use[i].second);
        it->second->stop.request_stop();
        streams_.erase(it);
    }
}

std::expected<std::string, std::string> PlayServer::play(File file, Progress on_progress) {
    std::uint64_t size = 0;
    std::string type = media_type(file.name);
    if (file.size) {
        size = *file.size;
    } else { // its size from rclone serve
        const auto& upstream = file.upstream.get();
        if (!upstream) return util::fail(upstream.error());
        const auto head = ask_upstream(*upstream, L"HEAD", {});
        if (!head || head->status != 200) return util::fail("the server's stream didn't answer");
        const auto length = header_of(head->request->h, WINHTTP_QUERY_CONTENT_LENGTH);
        if (const auto [end, err] = std::from_chars(length.data(), length.data() + length.size(), size);
            err != std::errc{} || end == length.data()) {
            return util::fail("the server's stream gave no size");
        }
    }

    auto s = std::make_shared<Stream>();
    {
        std::lock_guard lock(mutex_);
        if (listener_ == INVALID_SOCKET) {
            if (auto ok = listen(); !ok) return util::fail(ok.error());
        }
        forget_old();
        s->id = next_id_++;
        streams_[s->id] = s;
    }
    s->file = std::move(file);
    s->size = size;
    s->type = std::move(type);
    s->used = std::chrono::steady_clock::now();
    s->on_progress = std::move(on_progress);
    // At once: a long read from the start (its first bytes tell how far the player will read),
    // and the end. A small file is one read.
    s->tail_at = size > preview::warm_first + preview::warm_tail ? size - preview::warm_tail : size;
    if (size > 0) s->add(0, std::min(size, std::min(preview::warm_first, s->tail_at)));
    if (s->tail_at < size) s->add(s->tail_at, size);
    // An MKV may hold tens of MB (fonts) before its first video: two more reads go on at once,
    // without waiting for the first bytes to say so (a login takes seconds on a Storage Box).
    if (type.ends_with("matroska") || type == "video/webm") {
        const auto from = std::min(size, preview::warm_first);
        const auto until = std::min(s->tail_at, from + preview::warm_first);
        if (until > from + min_split) {
            const auto half = (until - from) / 2;
            s->add(from, from + half);
            s->add(from + half, until);
        }
    }
    if (size == 0) s->planned = true;

    const int pool_id = pool_ids + s->id;
    s->working = warm_reads;
    connections_.join(s->file.server, pool_id, warm_weight);
    for (int k = 0; k < warm_reads; ++k) {
        s->workers.emplace_back([this, raw = s.get()](std::stop_token) { warm(*raw); });
    }

    // The player starts once what it reads while opening is in: it then opens at once.
    {
        std::unique_lock lock(s->m);
        s->cv.wait_for(lock, s->stop.get_token(), ready_within, [&] { return s->ready() || s->working == 0; });
    }
    return std::format("{}{}/{}", base_, s->id, preview::url_path(s->file.name));
}

void PlayServer::warm(Stream& s) {
    const auto stop = s.stop.get_token();
    const int pool_id = pool_ids + s.id;
    for (;;) {
        std::size_t i = 0;
        std::uint64_t start = 0;
        std::uint64_t length = 0;
        {
            std::unique_lock lock(s.m);
            // The start first, then the end, then the rest of the start in order. Until the first
            // bytes are in, more reads may come.
            auto pick = [&]() -> std::optional<std::size_t> {
                std::optional<std::size_t> best;
                auto rank = [&](std::size_t k) {
                    const auto& g = s.segments[k];
                    return g.start == 0 ? 0 : (g.start >= s.tail_at ? 1 : 2);
                };
                for (std::size_t k = 0; k < s.segments.size(); ++k) {
                    if (s.segments[k].state != Segment::State::waiting) continue;
                    if (!best || rank(k) < rank(*best) ||
                        (rank(k) == rank(*best) && s.segments[k].start < s.segments[*best].start)) {
                        best = k;
                    }
                }
                return best;
            };
            std::optional<std::size_t> next;
            s.cv.wait(lock, stop, [&] {
                next = pick();
                return next.has_value() || s.planned;
            });
            if (!next || stop.stop_requested()) break;
            i = *next;
            s.segments[i].state = Segment::State::fetching;
            start = s.segments[i].start;
            length = s.segments[i].length;
        }
        auto report = [&] {
            Progress progress;
            std::pair<std::uint64_t, std::uint64_t> counts;
            {
                std::lock_guard lock(s.m);
                progress = s.on_progress;
                counts = s.needed();
            }
            if (progress) progress(counts.first, counts.second);
        };
        bool ok = false;
        if (connections_.acquire(s.file.server, pool_id, stop)) {
            proc::RunOptions quiet;
            quiet.detached = true;
            quiet.stop = stop;
            auto last_report = std::chrono::steady_clock::now();
            // The bytes as they arrive: served at once, and the first ones plan the rest.
            quiet.on_output = [&](std::string_view out) {
                {
                    std::lock_guard lock(s.m);
                    auto& d = s.data[i];
                    d.append(out.substr(0, static_cast<std::size_t>(std::min<std::uint64_t>(out.size(), length - d.size()))));
                    if (start == 0 && !s.planned && d.size() >= std::min<std::uint64_t>(plan_after, length)) s.plan(d);
                }
                s.cv.notify_all();
                if (const auto now = std::chrono::steady_clock::now(); now - last_report > std::chrono::milliseconds(100)) {
                    last_report = now;
                    report();
                }
            };
            auto ran = proc::run_capture(fetch::cat_argv(s.file.source, start, length), quiet);
            connections_.release(s.file.server, pool_id);
            std::lock_guard lock(s.m);
            ok = ran && ran->exit_code == 0 && s.data[i].size() == length;
        }
        {
            std::lock_guard lock(s.m);
            s.segments[i].state = ok ? Segment::State::done : Segment::State::failed;
            if (start == 0 && !s.planned) s.plan(s.data[i]); // with what came, if anything
        }
        s.cv.notify_all();
        report();
        if (stop.stop_requested()) break;
    }
    bool last = false;
    {
        std::lock_guard lock(s.m);
        last = --s.working == 0;
        if (last) s.on_progress = nullptr; // all in: its progress ends
    }
    s.cv.notify_all();
    if (last) connections_.leave(s.file.server, pool_id);
}

void PlayServer::serve(SOCKET c) {
    const DWORD timeout = 30000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::string request;
    char buf[4096];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 16384) {
        const int n = ::recv(c, buf, sizeof buf, 0);
        if (n <= 0) return;
        request.append(buf, static_cast<std::size_t>(n));
    }
    auto reply = [&](std::string_view status) {
        send_all(c, std::format("HTTP/1.1 {}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status));
    };
    // "GET /TOKEN/ID/name HTTP/1.1"
    const std::string_view text = request;
    const auto line_end = text.find("\r\n");
    const std::string_view line = text.substr(0, line_end);
    const auto sp1 = line.find(' ');
    const auto sp2 = line.find(' ', sp1 + 1);
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) return reply("400 Bad Request");
    const std::string_view method = line.substr(0, sp1);
    std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string prefix = "/" + token_ + "/";
    if (!target.starts_with(prefix)) return reply("404 Not Found");
    target.remove_prefix(prefix.size());
    int id = 0;
    const auto [id_end, id_err] = std::from_chars(target.data(), target.data() + target.size(), id);
    if (id_err != std::errc{}) return reply("404 Not Found");
    std::shared_ptr<Stream> s;
    {
        std::lock_guard lock(mutex_);
        if (const auto it = streams_.find(id); it != streams_.end()) s = it->second;
    }
    if (!s) return reply("404 Not Found");
    if (method != "GET" && method != "HEAD") return reply("405 Method Not Allowed");
    {
        std::lock_guard lock(s->m);
        s->used = std::chrono::steady_clock::now();
    }
    // The Range header, if any.
    std::string range;
    for (std::size_t at = line_end; at != std::string_view::npos && at < text.size();) {
        const auto next = text.find("\r\n", at + 2);
        const auto h = text.substr(at + 2, next == std::string_view::npos ? std::string_view::npos : next - at - 2);
        if (util::to_lower(h.substr(0, 6)) == "range:") range = std::string(util::trim(h.substr(6)));
        at = next;
    }
    const auto wanted = preview::requested_range(range, s->size);
    if (wanted.kind == preview::Requested::Kind::unsatisfiable) {
        send_all(c, std::format("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */{}\r\nContent-Length: 0\r\n"
                                "Connection: close\r\n\r\n",
                                s->size));
        return;
    }
    const bool part = wanted.kind == preview::Requested::Kind::part;
    const std::uint64_t first = part ? wanted.first : 0;
    const std::uint64_t last = part ? wanted.last : (s->size == 0 ? 0 : s->size - 1);
    const std::uint64_t length = s->size == 0 ? 0 : last - first + 1;
    std::string headers = std::format("HTTP/1.1 {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nAccept-Ranges: bytes\r\n",
                                      part ? "206 Partial Content" : "200 OK", s->type, length);
    if (part) headers += std::format("Content-Range: bytes {}-{}/{}\r\n", first, last, s->size);
    headers += "Connection: close\r\n\r\n";
    if (!send_all(c, headers) || method == "HEAD" || length == 0) return;

    const auto stop = s->stop.get_token();
    std::uint64_t pos = first;
    while (pos <= last && !closing_ && !stop.stop_requested()) {
        std::string piece;
        std::uint64_t upstream_last = last;
        {
            std::unique_lock lock(s->m);
            if (const auto in = s->holding(pos)) {
                // Fetched (or being fetched) by grab: from memory, as it arrives.
                const auto start = s->segments[*in].start;
                s->cv.wait(lock, stop, [&] { return s->data[*in].size() > pos - start || s->ended(*in); });
                if (stop.stop_requested()) return;
                const auto& d = s->data[*in];
                if (d.size() > pos - start) {
                    const auto from = static_cast<std::size_t>(pos - start);
                    const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(
                        {d.size() - from, last - pos + 1, send_piece}));
                    piece = d.substr(from, n);
                } else { // the read failed before this byte: rclone serve answers for the rest of it
                    upstream_last = std::min(last, start + s->segments[*in].length - 1);
                }
            } else {
                upstream_last = std::min(last, s->next_after(pos, last + 1) - 1);
            }
        }
        if (!piece.empty()) {
            if (!send_all(c, piece)) return;
            pos += piece.size();
            continue;
        }
        // From rclone serve: [pos, upstream_last].
        const auto& upstream = s->file.upstream.get();
        if (!upstream) return;
        auto up = ask_upstream(*upstream, L"GET", std::format(L"Range: bytes={}-{}", pos, upstream_last));
        if (!up || (up->status != 206 && up->status != 200)) return;
        if (up->status == 200 && pos != 0) return; // it ignored the range
        std::uint64_t left = upstream_last - pos + 1;
        std::string chunk;
        while (left > 0 && !closing_) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(up->request->h, &available) || available == 0) return;
            chunk.resize(static_cast<std::size_t>(std::min<std::uint64_t>({available, left, send_piece})));
            DWORD read = 0;
            if (!WinHttpReadData(up->request->h, chunk.data(), static_cast<DWORD>(chunk.size()), &read) || read == 0) return;
            if (!send_all(c, std::string_view(chunk).substr(0, read))) return;
            left -= read;
            pos += read;
        }
        if (left > 0) return;
    }
}

} // namespace grab::gui
