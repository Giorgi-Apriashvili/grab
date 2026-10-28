#include "play_stream.hpp"

#include "conn_budget.hpp"
#include "preview.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_read.hpp"
#include "util.hpp"

#include <ws2tcpip.h> // IWYU pragma: keep (the socket API)

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <limits>
#include <random>
#include <system_error>

namespace grab::play {

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::size_t block = std::size_t{1} << 20;       // the cache's unit
using Block = std::array<char, block>;
constexpr std::uint64_t mkv_head = std::uint64_t{64} << 10; // the first bytes that tell how far an MKV's start goes
constexpr std::size_t send_piece = std::size_t{1} << 20;
constexpr int high = 4;                                   // Play's weight among the server's users

std::uint64_t plus(std::uint64_t a, std::uint64_t b) { // without wrapping round
    return a > std::numeric_limits<std::uint64_t>::max() - b ? std::numeric_limits<std::uint64_t>::max() : a + b;
}

// 128 random bits as hex: the server's path, so other programs on this PC can't guess it.
std::string random_token() {
    std::random_device rd;
    constexpr std::string_view hex = "0123456789abcdef";
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

bool send_all(SOCKET s, std::string_view data) {
    while (!data.empty()) {
        const int n = ::send(s, data.data(), static_cast<int>(std::min(data.size(), send_piece)), 0);
        if (n <= 0) return false;
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return true;
}

bool token_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
           c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' || c == '^' ||
           c == '`' || c == '|' || c == '~';
}

// A read that failed, in a few words for "Can't play x: ...".
std::string why(const ssh::ReadError& e, std::string_view server) {
    switch (e.kind) {
    case ssh::ReadFailure::stopped: return "stopped";
    case ssh::ReadFailure::login: {
        std::string text = std::format("can't log in to {}: {}", server, e.message);
        // A failure that can't pass by itself is kept until Test, Trust or a save (acquire_remembered).
        if (e.message.find("host key") != std::string::npos) text += ". If the key is right, use Trust… in Settings";
        else if (e.login == budget::Failure::fatal) text += ". Test the server in Settings to try again";
        return text;
    }
    case ssh::ReadFailure::status:
        if (e.code == sftp::Code::no_such_file) return std::format("not found on {}", server);
        return std::format("{} answered: {}", server, e.message);
    case ssh::ReadFailure::not_file: return "not a regular file";
    case ssh::ReadFailure::lost: break;
    }
    return std::format("lost the connection to {}: {}", server, e.message);
}

} // namespace

// ---- Ranges -----------------------------------------------------------------------------------

void Ranges::add(std::uint64_t start, std::uint64_t end) {
    if (start >= end) return;
    auto it = r_.upper_bound(start);
    if (it != r_.begin()) {
        const auto before = std::prev(it);
        if (before->second >= start) {
            start = before->first;
            end = std::max(end, before->second);
            it = r_.erase(before);
        }
    }
    while (it != r_.end() && it->first <= end) {
        end = std::max(end, it->second);
        it = r_.erase(it);
    }
    r_.emplace(start, end);
}

void Ranges::remove(std::uint64_t start, std::uint64_t end) {
    if (start >= end) return;
    auto it = r_.upper_bound(start);
    if (it != r_.begin()) --it;
    while (it != r_.end() && it->first < end) {
        const auto [a, b] = *it;
        if (b <= start) {
            ++it;
            continue;
        }
        it = r_.erase(it);
        if (a < start) r_.emplace(a, start);
        if (b > end) {
            r_.emplace(end, b);
            break;
        }
    }
}

std::uint64_t Ranges::until(std::uint64_t pos) const {
    auto it = r_.upper_bound(pos);
    if (it == r_.begin()) return pos;
    --it;
    return it->second > pos ? it->second : pos;
}

std::uint64_t Ranges::next_after(std::uint64_t pos, std::uint64_t limit) const {
    const auto it = r_.upper_bound(pos);
    return it == r_.end() ? limit : std::min(limit, it->first);
}

std::uint64_t Ranges::bytes_in(std::uint64_t start, std::uint64_t end) const {
    if (start >= end) return 0;
    std::uint64_t n = 0;
    auto it = r_.upper_bound(start);
    if (it != r_.begin()) --it;
    for (; it != r_.end() && it->first < end; ++it) {
        const auto a = std::max(it->first, start);
        const auto b = std::min(it->second, end);
        if (b > a) n += b - a;
    }
    return n;
}

std::optional<std::pair<std::uint64_t, std::uint64_t>> gap(const Ranges& a, const Ranges& b, std::uint64_t start,
                                                         std::uint64_t end) {
    std::uint64_t pos = start;
    for (;;) {
        if (pos >= end) return std::nullopt;
        const auto past = std::max(a.until(pos), b.until(pos));
        if (past == pos) break;
        pos = past;
    }
    return std::pair{pos, std::min({end, a.next_after(pos, end), b.next_after(pos, end)})};
}

// ---- HTTP -------------------------------------------------------------------------------------

std::optional<Request> parse_request(std::string_view head) {
    if (head.ends_with("\r\n\r\n")) head.remove_suffix(2);
    auto line_end = head.find("\r\n");
    if (line_end == std::string_view::npos) return std::nullopt;
    // "GET /TOKEN/ID/name HTTP/1.1"
    const std::string_view line = head.substr(0, line_end);
    const auto sp1 = line.find(' ');
    const auto sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
    if (sp2 == std::string_view::npos || line.find(' ', sp2 + 1) != std::string_view::npos) return std::nullopt;
    Request r;
    r.method = line.substr(0, sp1);
    r.target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::string_view version = line.substr(sp2 + 1);
    if (r.method.empty() || !std::ranges::all_of(r.method, token_char) || !r.target.starts_with('/') ||
        !version.starts_with("HTTP/1.")) {
        return std::nullopt;
    }
    for (std::size_t at = line_end + 2; at < head.size();) {
        auto next = head.find("\r\n", at);
        if (next == std::string_view::npos) next = head.size();
        const std::string_view h = head.substr(at, next - at);
        at = next + 2;
        const auto colon = h.find(':');
        // No folded lines, no name without a value, no space before the colon.
        if (colon == std::string_view::npos || colon == 0 || h.starts_with(' ') || h.starts_with('\t') ||
            !std::ranges::all_of(h.substr(0, colon), token_char)) {
            return std::nullopt;
        }
        const std::string name = util::to_lower(h.substr(0, colon));
        const std::string value(util::trim(h.substr(colon + 1)));
        if (name == "host") {
            if (r.host) return std::nullopt; // two: which one is meant can't be told
            r.host = value;
        } else if (name == "range") {
            r.range = value;
        }
    }
    return r;
}

Answer answer(const Request& request, std::uint64_t size, std::string_view content_type) {
    Answer a;
    const auto wanted = request.range ? preview::requested_range(*request.range, size) : preview::Requested{};
    switch (wanted.kind) {
    case preview::Requested::Kind::unsatisfiable:
        a.status = 416;
        a.head = std::format("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */{}\r\nContent-Length: 0\r\n"
                             "Connection: close\r\n\r\n",
                             size);
        return a;
    case preview::Requested::Kind::part:
        a.status = 206;
        a.first = wanted.first;
        a.length = wanted.last - wanted.first + 1; // last < size: no wrapping round
        break;
    case preview::Requested::Kind::whole:
        a.length = size;
        break;
    }
    a.head = std::format("HTTP/1.1 {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nAccept-Ranges: bytes\r\n",
                         a.status == 206 ? "206 Partial Content" : "200 OK", content_type, a.length);
    if (a.status == 206) a.head += std::format("Content-Range: bytes {}-{}/{}\r\n", a.first, a.first + a.length - 1, size);
    a.head += "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    return a;
}

std::string bare(std::string_view status) {
    return std::format("HTTP/1.1 {}\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
}

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

bool same_token(std::string_view a, std::string_view b) {
    unsigned diff = a.size() == b.size() ? 0U : 1U;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(i < b.size() ? b[i] : 0);
    }
    return diff == 0;
}

// ---- Plan -------------------------------------------------------------------------------------

void Plan::warm(std::uint64_t head, std::uint64_t tail_at) {
    head_ = std::min(head, s_.size);
    tail_at_ = std::min(tail_at, s_.size);
    warming_ = true;
}

bool Plan::pinned(std::uint64_t start, std::uint64_t end) const {
    return start < head_ || (tail_at_ < s_.size && end > tail_at_);
}

std::pair<std::uint64_t, std::uint64_t> Plan::warm_progress() const {
    const std::uint64_t last = end();
    const std::uint64_t head = std::min(head_, last);
    const std::uint64_t tail = std::max(std::min(tail_at_, last), head); // where the end's part starts
    return {have.bytes_in(0, head) + have.bytes_in(tail, last), head + (last - tail)};
}

bool Plan::warm_in() const {
    const auto [done, total] = warm_progress();
    return done >= total;
}

int Plan::add_reader(std::uint64_t pos, std::uint64_t last) {
    readers_.push_back(Reader{.id = next_reader_, .pos = pos, .last = last});
    return next_reader_++;
}

void Plan::move_reader(int id, std::uint64_t pos) {
    for (auto& r : readers_) {
        if (r.id == id) r.pos = pos;
    }
}

void Plan::remove_reader(int id) { std::erase_if(readers_, [id](const Reader& r) { return r.id == id; }); }

void Plan::ended_at(std::uint64_t at) { eof_ = std::min(eof_, at); }

std::uint64_t Plan::window_end(const Reader& r) const {
    return std::min({plus(r.last, 1), plus(r.pos, s_.ahead), end()});
}

std::optional<Plan::Piece> Plan::pick(const Lanes& lanes, bool room) const {
    const std::uint64_t last = end();
    constexpr auto none = std::numeric_limits<std::uint64_t>::max();
    // A request waiting for bytes nobody fetches.
    if (!lanes.busy || lanes.idle_others == 0) {
        for (const auto& r : readers_) {
            if (r.pos >= std::min(plus(r.last, 1), last)) continue;
            if (have.until(r.pos) > r.pos || taken.until(r.pos) > r.pos) continue; // in, or on its way
            const auto stop = std::min({plus(r.last, 1), last, plus(r.pos, s_.seek_piece), have.next_after(r.pos, none),
                                        taken.next_after(r.pos, none)});
            return Piece{.start = r.pos, .end = stop, .kind = Kind::waiting};
        }
    }
    // The warm part, dealt out: its very start first, then the end, then the rest of the start.
    if (warming_) {
        const std::uint64_t head = std::min(head_, last);
        const std::uint64_t tail = std::max(std::min(tail_at_, last), head);
        auto g = gap(have, taken, 0, head);
        if (!g || g->first != 0) {
            if (auto end_part = gap(have, taken, tail, last)) g = end_part;
        }
        if (g) {
            // What nobody has taken yet, shared among the connections: pieces shrink near the end.
            std::uint64_t untaken = 0;
            for (const auto& [from, to] : {std::pair{std::uint64_t{0}, head}, std::pair{tail, last}}) {
                for (auto at = gap(have, taken, from, to); at; at = gap(have, taken, at->second, to)) {
                    untaken += at->second - at->first;
                }
            }
            const auto connections = static_cast<std::uint64_t>(std::max(lanes.reading, 1));
            const std::uint64_t most = std::max(s_.piece, s_.min_piece);
            const std::uint64_t n = std::min(std::clamp(untaken / (2 * connections), s_.min_piece, most), g->second - g->first);
            return Piece{.start = g->first, .end = g->first + n, .kind = Kind::warm};
        }
    }
    // Read-ahead; the last idle connection stays for a seek.
    if (!room || (!lanes.busy && lanes.idle_others == 0 && lanes.reading > 1)) return std::nullopt;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> best;
    std::uint64_t nearest = none;
    for (const auto& r : readers_) {
        std::uint64_t from = r.pos;
        // A busy connection leaves a waiting request's first bytes to the idle one taking them.
        if (lanes.busy && lanes.idle_others > 0 && have.until(r.pos) == r.pos && taken.until(r.pos) == r.pos) {
            from = std::min(window_end(r), plus(r.pos, s_.seek_piece));
        }
        const auto g = gap(have, taken, from, window_end(r));
        if (g && g->first - r.pos < nearest) {
            nearest = g->first - r.pos;
            best = g;
        }
    }
    if (!best) return std::nullopt;
    return Piece{.start = best->first, .end = std::min(best->second, plus(best->first, s_.piece)), .kind = Kind::ahead};
}

std::optional<Plan::Piece> Plan::next(const Lanes& lanes, bool room) {
    auto p = pick(lanes, room);
    if (p) taken.add(p->start, p->end);
    return p;
}

bool Plan::any_work(bool room) const { return pick(Lanes{.busy = false, .idle_others = 1, .reading = 1}, room).has_value(); }

bool Plan::wanted(std::uint64_t pos) const {
    if (pos >= end()) return false;
    if (warming_ && (pos < head_ || pos >= tail_at_)) return true;
    return std::ranges::any_of(readers_, [&](const Reader& r) {
        return pos >= r.pos && pos < plus(window_end(r), s_.piece);
    });
}

std::vector<std::uint64_t> Plan::evictable(const std::vector<std::uint64_t>& starts, std::uint64_t length) const {
    struct Candidate {
        bool ahead = false;          // in front of every request (behind one goes first)
        std::uint64_t distance = 0;  // from the nearest request: the farthest goes first
        std::uint64_t start = 0;
    };
    std::vector<Candidate> out;
    for (const auto start : starts) {
        const auto stop = plus(start, length);
        if (pinned(start, stop) || taken.bytes_in(start, stop) > 0) continue;
        const bool in_window = std::ranges::any_of(readers_, [&](const Reader& r) {
            return stop > r.pos && start < window_end(r);
        });
        if (in_window) continue;
        Candidate c{.ahead = true, .distance = std::numeric_limits<std::uint64_t>::max(), .start = start};
        for (const auto& r : readers_) {
            if (stop <= r.pos) {
                if (c.ahead || r.pos - stop < c.distance) c.distance = r.pos - stop;
                c.ahead = false;
            } else if (c.ahead) {
                c.distance = std::min(c.distance, start - std::min(start, window_end(r)));
            }
        }
        out.push_back(c);
    }
    std::ranges::sort(out, [](const Candidate& a, const Candidate& b) {
        if (a.ahead != b.ahead) return !a.ahead;
        return a.distance > b.distance;
    });
    std::vector<std::uint64_t> order;
    order.reserve(out.size());
    for (const auto& c : out) order.push_back(c.start);
    return order;
}

// ---- a stream ---------------------------------------------------------------------------------

struct Server::Stream {
    struct Lane;

    Stream(Server& owner, File f, std::uint64_t file_size, int seat);
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    Server& server;
    int id = 0;
    File file;
    std::string type;
    std::uint64_t size = 0;
    int seat_id = 0; // its user id in the server's connections (fetch::Connections)

    std::mutex m;
    std::condition_variable_any cv; // bytes in, work to do, a request moved or ended
    Plan plan;
    std::map<std::uint64_t, std::unique_ptr<Block>> blocks; // the cache, by block
    bool decided = false;                // the warm part's start follows the first bytes
    std::optional<std::string> failure;  // why reads stopped; the next request tries again
    int breaks = 0;                      // connections that broke in a row without bytes
    int requests = 0;                    // open
    Clock::time_point last_request;      // the last request ended (or the warm part came in)
    Clock::time_point last_in;           // bytes came
    Clock::time_point used;              // a request began or ended
    bool joined = false;                 // in the connections, under seat_id
    int seat_users = 0;                  // lanes holding a seat or waiting for one
    std::vector<std::unique_ptr<Lane>> lanes;
    bool running = false;                // lanes started
    bool halting = false;                // the keeper is stopping the lanes to free the data
    bool abandoned = false;              // not wanted: its Play stopped, or its data let go (forget_old)
    bool forgotten = false;              // beyond Options::ids: it goes
    std::uint64_t ahead_taken = 0;       // bytes of read-ahead handed out (Stats)

    // Under m.
    [[nodiscard]] bool room() const { return blocks.size() * block < server.options_.stream_cap; }
    [[nodiscard]] std::uint64_t cached() const { return blocks.size() * block; }
    [[nodiscard]] Plan::Lanes lane_stats(const Lane* asking, bool busy) const;
    void start_lanes(bool first_seated); // under m, not running
    void store(std::uint64_t at, std::string_view bytes);
    [[nodiscard]] std::string copy(std::uint64_t pos, std::uint64_t n) const;
    // Blocks go until one more piece fits under the cap: room() stays true while some may go.
    void evict();
    void fail(std::string why);
    void leave_if_unused(); // no lane holds a seat or waits for one: out of the connections
    // Lanes out, for the caller to stop and join outside the lock.
    std::vector<std::unique_ptr<Lane>> take_lanes();
};

// One connection of a stream: a seat, a pooled connection and its own handle on the file, kept
// while the stream is in use; READs pipelined over the pieces the plan hands it.
struct Server::Stream::Lane {
    explicit Lane(Stream& stream, bool seated_already) : s(stream), seated(seated_already) {}
    void start() {
        thread = std::jthread([this](const std::stop_token& stop) { run(stop); });
    }

    Stream& s;
    // Under s.m:
    bool connected = false;
    bool busy = false;
    // Its thread's own:
    bool seated = false;
    std::optional<ssh::Pool::Lease> lease;
    std::string handle;
    Clock::time_point retry_after{}; // a login failed: none before then
    Clock::time_point last_reply{};
    std::uint32_t size = 0;          // the next READ's
    std::uint64_t pos = 0;           // the piece's next byte to ask for
    std::uint64_t piece_end = 0;
    Plan::Kind kind = Plan::Kind::ahead;
    std::map<std::uint64_t, std::uint64_t> asked; // READs on their way
    std::uint64_t in_flight = 0;
    int reads = 0;
    bool yielded = false;
    std::jthread thread; // last: joined first

    void run(const std::stop_token& stop);
    bool connect(const std::stop_token& stop);
    bool read(const std::stop_token& stop); // false: drop the connection
    void fill(const std::stop_token& stop);
    bool take();
    [[nodiscard]] std::uint64_t cap() const;
    void ask(std::uint64_t at, std::uint64_t n);
    void give_back(); // after a break: its bytes not in yet, for the others
    // The connection back to the pool (closed when `drop`: replies left on it), then the seat
    // (`seat_gone`: handed back already, yield_if_over_share).
    void release(bool drop, bool seat_gone);
    void seat_back(bool gone = false);
};

Server::Stream::Stream(Server& owner, File f, std::uint64_t file_size, int seat)
    : server(owner), file(std::move(f)), type(media_type(file.name)), size(file_size), seat_id(seat),
      plan(Plan::Settings{.size = file_size, .ahead = file.ahead}) {}

Server::Stream::~Stream() {
    for (auto& lane : lanes) lane->thread.request_stop();
    cv.notify_all();
    lanes.clear();
}

Plan::Lanes Server::Stream::lane_stats(const Lane* asking, bool busy) const {
    Plan::Lanes l{.busy = busy, .idle_others = 0, .reading = 0};
    for (const auto& lane : lanes) {
        if (!lane->connected) continue;
        ++l.reading;
        if (lane.get() != asking && !lane->busy) ++l.idle_others;
    }
    l.reading = std::max(l.reading, 1);
    return l;
}

void Server::Stream::start_lanes(bool first_seated) {
    lanes.clear();
    for (int i = 0; i < std::max(file.connections, 1); ++i) lanes.push_back(std::make_unique<Lane>(*this, first_seated && i == 0));
    for (auto& lane : lanes) lane->start();
    running = true;
}

void Server::Stream::store(std::uint64_t at, std::string_view bytes) {
    const std::uint64_t from = at;
    while (!bytes.empty() && at < size) {
        const std::uint64_t index = at / block;
        const auto offset = static_cast<std::size_t>(at % block);
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>({block - offset, bytes.size(), size - at}));
        auto& b = blocks[index];
        if (!b) b = std::make_unique_for_overwrite<Block>();
        std::memcpy(b->data() + offset, bytes.data(), n);
        at += n;
        bytes.remove_prefix(n);
    }
    plan.have.add(from, at);
    plan.taken.remove(from, at);
    last_in = Clock::now();
    breaks = 0;
    // The first bytes say how far the start goes: an MKV up to its first video cluster.
    if (!decided && plan.have.covers(0, std::min(mkv_head, size))) {
        decided = true;
        if (plan.warming()) plan.warm(preview::warm_start(copy(0, std::min(mkv_head, size)), size), plan.tail_at());
    }
    if (decided && plan.warming() && plan.warm_in()) plan.warm_done();
    if (cached() > server.options_.stream_cap) evict();
    cv.notify_all();
}

std::string Server::Stream::copy(std::uint64_t pos, std::uint64_t n) const {
    std::string out;
    out.reserve(static_cast<std::size_t>(n));
    while (n > 0) {
        const auto it = blocks.find(pos / block);
        if (it == blocks.end()) break;
        const auto offset = static_cast<std::size_t>(pos % block);
        const auto k = static_cast<std::size_t>(std::min<std::uint64_t>(block - offset, n));
        out.append(it->second->data() + offset, k);
        pos += k;
        n -= k;
    }
    return out;
}

void Server::Stream::evict() {
    // Down to a piece under the cap, not to the cap: a cache left exactly full would have no room
    // for read-ahead, so no store() would come to evict again.
    const std::uint64_t cap = server.options_.stream_cap;
    const std::uint64_t target = cap - std::min(cap, plan.settings().piece);
    std::vector<std::uint64_t> starts;
    starts.reserve(blocks.size());
    for (const auto& [index, b] : blocks) starts.push_back(index * block);
    for (const auto start : plan.evictable(starts, block)) {
        if (cached() <= target) break;
        blocks.erase(start / block);
        plan.have.remove(start, start + block);
    }
}

void Server::Stream::fail(std::string why) {
    if (!failure) failure = std::move(why);
    cv.notify_all();
}

void Server::Stream::leave_if_unused() {
    if (joined && seat_users == 0) {
        server.connections_.leave(file.server, seat_id);
        joined = false;
    }
}

std::vector<std::unique_ptr<Server::Stream::Lane>> Server::Stream::take_lanes() {
    auto out = std::move(lanes);
    lanes.clear();
    for (auto& lane : out) lane->thread.request_stop();
    cv.notify_all();
    return out;
}

void Server::Stream::Lane::run(const std::stop_token& stop) {
    const auto& o = s.server.options_;
    while (!stop.stop_requested()) {
        bool work = false;
        bool due = false; // the stream is idle: the connection and the seat go back
        {
            std::unique_lock lock(s.m);
            busy = false;
            connected = lease.has_value();
            auto check = [&] {
                const auto now = Clock::now();
                // A warm part still coming in holds them, unless its reads failed (no bytes come).
                due = (lease || seated) && s.requests == 0 && (!s.plan.warming() || s.failure) &&
                      now - s.last_request >= o.release_after;
                if (s.failure) work = false;
                else if (lease) work = s.plan.would(s.lane_stats(this, false), s.room());
                else work = now >= retry_after && s.plan.any_work(s.room());
                return work || due;
            };
            if (!s.cv.wait_for(lock, stop, std::chrono::milliseconds(250), check)) continue;
            if (stop.stop_requested()) break;
        }
        if (!work) {
            if (due) release(false, false);
            continue;
        }
        if (!lease) {
            connect(stop);
            continue; // with the connection, the work again
        }
        try {
            if (!read(stop)) { // stopped with more on its way than is worth waiting for
                give_back();
                lease->broken();
                lease.reset();
                handle.clear();
            }
        } catch (const sftp::Broken& e) {
            give_back();
            lease->broken();
            lease.reset();
            handle.clear();
            std::scoped_lock lock(s.m);
            // A connection the server closed is replaced (the next login); one after another
            // breaking without bytes ends the reads until the next request.
            if (++s.breaks >= 3) s.fail(std::format("lost the connection to {}: {}", s.file.server, e.what()));
        }
        if (yielded) { // its seat went to a download waiting for its share
            yielded = false;
            release(false, true);
        }
    }
    // Stopped: what was on its way is in (or the connection was dropped); the handle closes.
    release(true, false);
}

bool Server::Stream::Lane::connect(const std::stop_token& stop) {
    auto& c = s.server.connections_;
    if (!seated) {
        {
            std::scoped_lock lock(s.m);
            if (!s.joined) {
                c.join(s.file.server, s.seat_id, high);
                s.joined = true;
            }
            ++s.seat_users;
        }
        // As a download waits for its connection: downloads above their share step aside.
        if (!c.acquire(s.file.server, s.seat_id, stop, false)) {
            std::scoped_lock lock(s.m);
            --s.seat_users;
            s.leave_if_unused();
            return false;
        }
        seated = true;
        // The seat may have come long after the stream stopped needing it: no login for nothing.
        bool needed = true;
        {
            std::scoped_lock lock(s.m);
            needed = !s.failure && s.plan.any_work(s.room());
        }
        if (!needed) {
            seat_back();
            return false;
        }
    }
    for (bool retried = false;;) {
        try {
            lease = s.file.pool->acquire_remembered(stop, ssh::interactive_retry, s.file.logins_at_once);
        } catch (const ssh::Error& e) {
            seat_back();
            std::scoped_lock lock(s.m);
            retry_after = Clock::now() + s.server.options_.stalled_after;
            // Fatal for the stream only while no connection reads for it.
            if (std::ranges::none_of(s.lanes, [](const auto& l) { return l->connected; })) {
                s.fail(why(ssh::ReadError{.kind = ssh::ReadFailure::login, .message = e.what(), .login = e.kind}, s.file.server));
            }
            return false;
        }
        if (!lease) {
            seat_back();
            return false;
        }
        try {
            auto opened = sftp::open_file((*lease)->sftp(), s.file.path);
            if (!opened) {
                lease.reset();
                seat_back();
                std::scoped_lock lock(s.m);
                retry_after = Clock::now() + s.server.options_.stalled_after;
                s.fail(why(ssh::ReadError{.kind = ssh::ReadFailure::status, .message = sftp::describe(opened.error()),
                                          .code = opened.error().code},
                           s.file.server));
                return false;
            }
            handle = std::move(*opened);
        } catch (const sftp::Unavailable& e) {
            lease->broken();
            lease.reset();
            seat_back();
            std::scoped_lock lock(s.m);
            s.fail(std::format("{}: {}", s.file.server, e.what()));
            return false;
        } catch (const sftp::Broken& e) {
            // A reused connection the server closed while it sat idle: the next one, once.
            lease->broken();
            const bool again = lease->reused() && !std::exchange(retried, true);
            lease.reset();
            if (again) continue;
            seat_back();
            std::scoped_lock lock(s.m);
            retry_after = Clock::now() + std::chrono::seconds(1);
            if (++s.breaks >= 3) s.fail(std::format("lost the connection to {}: {}", s.file.server, e.what()));
            return false;
        }
        std::scoped_lock lock(s.m);
        connected = true;
        last_reply = Clock::time_point{}; // a new connection starts small
        return true;
    }
}

bool Server::Stream::Lane::read(const std::stop_token& stop) {
    const auto& o = s.server.options_;
    auto& client = (*lease)->sftp();
    // An idle connection starts TCP again from a small window: its first READ is small.
    if (Clock::now() - last_reply >= o.idle_restart) size = std::clamp<std::uint32_t>(o.first_read, 1, sftp::max_read);
    for (;;) {
        if (stop.stop_requested()) break;
        fill(stop);
        if (reads == 0) break;
        client.pump();
    }
    // SFTP READs can't be called back: more than drop_over on its way (or the window closing)
    // drops the connection, less is waited for.
    if (reads > 0 && (in_flight > o.drop_over || s.server.closing_)) return false;
    while (reads > 0) client.pump();
    return true;
}

void Server::Stream::Lane::fill(const std::stop_token& stop) {
    const int depth = std::max(s.server.options_.depth, 1);
    while (!stop.stop_requested() && reads < depth) {
        if (pos >= piece_end) {
            if (!take()) return;
            continue;
        }
        {
            std::scoped_lock lock(s.m);
            // Nobody needs these bytes any more (the request that wanted them went): not asked for.
            if (s.failure || !s.plan.wanted(pos)) {
                s.plan.taken.remove(pos, piece_end);
                piece_end = pos;
                continue;
            }
        }
        const std::uint64_t n = std::min<std::uint64_t>(size, piece_end - pos);
        if (reads > 0 && in_flight + n > cap()) return;
        ask(pos, n);
        pos += n;
        size = std::min(size * 2, sftp::max_read);
    }
}

bool Server::Stream::Lane::take() {
    if (yielded) return false;
    std::scoped_lock lock(s.m);
    if (s.failure) {
        busy = reads > 0;
        return false;
    }
    auto p = s.plan.next(s.lane_stats(this, reads > 0), s.room());
    if (p && p->kind == Plan::Kind::ahead &&
        s.server.connections_.yield_if_over_share(s.file.server, s.seat_id)) {
        // Over its share while a download waits for its own: the seat goes back (read-ahead only).
        s.plan.taken.remove(p->start, p->end);
        yielded = true;
        p.reset();
    }
    busy = p.has_value() || reads > 0;
    if (!p) return false;
    if (p->kind == Plan::Kind::ahead) s.ahead_taken += p->end - p->start;
    pos = p->start;
    piece_end = p->end;
    kind = p->kind;
    return true;
}

std::uint64_t Server::Stream::Lane::cap() const {
    const auto& o = s.server.options_;
    const std::uint64_t deepest = std::uint64_t{sftp::max_read} * static_cast<std::uint64_t>(std::max(o.depth, 1));
    if (kind != Plan::Kind::warm) return deepest;
    // The warm part: its share of what's left, so the slowest connection doesn't hold the end.
    std::scoped_lock lock(s.m);
    const auto [done, total] = s.plan.warm_progress();
    const auto connections = static_cast<std::uint64_t>(s.lane_stats(this, true).reading);
    return std::clamp<std::uint64_t>((total - std::min(done, total)) / connections, sftp::max_read, deepest);
}

void Server::Stream::Lane::ask(std::uint64_t at, std::uint64_t n) {
    ++reads;
    in_flight += n;
    asked[at] = n;
    (*lease)->sftp().read(handle, at, static_cast<std::uint32_t>(n), [this, at, n](std::expected<std::string_view, sftp::Status> r) {
        --reads;
        in_flight -= n;
        asked.erase(at);
        last_reply = Clock::now();
        std::uint64_t got = 0;
        {
            std::scoped_lock lock(s.m);
            if (!r) {
                s.plan.taken.remove(at, at + n);
                // EOF: the file ends at `at` (shorter than STAT said).
                if (r.error().code == sftp::Code::eof) s.plan.ended_at(at);
                else s.fail(std::format("{} answered: {}", s.file.server, sftp::describe(r.error())));
                s.cv.notify_all();
                return;
            }
            got = std::min<std::uint64_t>(r->size(), n);
            s.store(at, r->substr(0, static_cast<std::size_t>(got)));
        }
        if (got < n) ask(at + got, n - got); // a short read: the rest, exactly (still taken)
    });
}

void Server::Stream::Lane::give_back() {
    {
        std::scoped_lock lock(s.m);
        if (pos < piece_end) s.plan.taken.remove(pos, piece_end);
        for (const auto& [at, n] : asked) s.plan.taken.remove(at, at + n);
        busy = false;
        connected = false;
    }
    pos = piece_end;
    asked.clear();
    reads = 0;
    in_flight = 0;
    s.cv.notify_all();
}

void Server::Stream::Lane::release(bool drop, bool seat_gone) {
    if (lease) {
        // Closed rather than kept: replies still on their way, the window closing (no CLOSE to wait
        // for: the server closes the handle with the connection), or engine = rclone, whose rclone
        // can't see the pool's idle connections while a Storage Box counts them.
        if ((drop && (reads > 0 || s.server.closing_)) || s.file.close_on_release) {
            lease->broken();
        } else {
            try {
                if (!handle.empty()) {
                    // A handle that won't close leaves nothing behind that matters here.
                    [[maybe_unused]] auto closed = sftp::close_file((*lease)->sftp(), handle);
                }
            } catch (const sftp::Broken&) {
                lease->broken();
            }
        }
        lease.reset(); // back to the pool before the seat, for whoever takes the seat next
        handle.clear();
    }
    {
        std::scoped_lock lock(s.m);
        connected = false;
        busy = false;
    }
    seat_back(seat_gone);
}

void Server::Stream::Lane::seat_back(bool gone) {
    if (!seated) return;
    seated = false;
    if (!gone) s.server.connections_.release(s.file.server, s.seat_id);
    std::scoped_lock lock(s.m);
    --s.seat_users;
    s.leave_if_unused();
}

// ---- the server -------------------------------------------------------------------------------

Server::Server(fetch::Connections& connections, Options options)
    : connections_(connections), options_(options) {}

Server::~Server() {
    std::vector<std::shared_ptr<Stream>> streams;
    {
        std::scoped_lock lock(mutex_);
        closing_ = true;
        if (listener_ != INVALID_SOCKET) closesocket(listener_); // accept() returns
        listener_ = INVALID_SOCKET;
        // Their sends and waits end: a later send fails after the shutdown, but a send or select
        // already blocked (a paused player that stopped reading) wakes only on CancelIoEx. Under
        // mutex_, so a client thread can't close the socket meanwhile and its number be reused.
        for (const auto& c : clients_) {
            if (c.socket == INVALID_SOCKET) continue;
            shutdown(c.socket, SD_BOTH);
            CancelIoEx(reinterpret_cast<HANDLE>(c.socket), nullptr);
        }
        for (const auto& [id, s] : streams_) streams.push_back(s);
    }
    for (const auto& s : streams) {
        std::scoped_lock lock(s->m);
        s->cv.notify_all();
    }
    changed_.notify_all();
    if (acceptor_.joinable()) acceptor_.join();
    keeper_.request_stop();
    if (keeper_.joinable()) keeper_.join();
    std::list<Client> clients;
    {
        std::scoped_lock lock(mutex_);
        clients.swap(clients_);
    }
    clients.clear(); // joined
    // The streams' lanes stop and are joined; their connections and seats go back.
    for (const auto& s : streams) {
        std::vector<std::unique_ptr<Stream::Lane>> lanes;
        {
            std::scoped_lock lock(s->m);
            lanes = s->take_lanes();
        }
        lanes.clear();
    }
    streams.clear();
    {
        std::scoped_lock lock(mutex_);
        streams_.clear();
    }
    if (winsock_) WSACleanup();
}

std::expected<void, std::string> Server::listen() {
    if (!winsock_) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return util::fail("cannot use the network (WSAStartup)");
        winsock_ = true;
    }
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
        return util::failf("cannot start the player's stream: {}", std::system_category().message(error));
    }
    listener_ = l;
    port_ = ntohs(at.sin_port);
    token_ = random_token();
    base_ = std::format("http://127.0.0.1:{}/{}/", port_, token_);
    acceptor_ = std::jthread([this] { accept_loop(); });
    keeper_ = std::jthread([this](const std::stop_token& stop) { housekeep(stop); });
    return {};
}

std::string Server::base() {
    std::scoped_lock lock(mutex_);
    return base_;
}

void Server::accept_loop() {
    SOCKET listener = INVALID_SOCKET;
    {
        std::scoped_lock lock(mutex_);
        listener = listener_;
    }
    for (;;) {
        const SOCKET c = ::accept(listener, nullptr, nullptr);
        if (c == INVALID_SOCKET) {
            if (closing_) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        std::scoped_lock lock(mutex_);
        if (closing_) {
            closesocket(c);
            return;
        }
        const auto open = std::ranges::count_if(clients_, [](const Client& k) { return !k.done; });
        if (static_cast<std::size_t>(open) >= options_.max_clients) { // more at once: closed (review G49)
            closesocket(c);
            continue;
        }
        Client& client = clients_.emplace_back();
        client.socket = c;
        client.thread = std::jthread([this, &client] {
            serve(client.socket);
            std::scoped_lock done(mutex_);
            closesocket(client.socket);
            client.socket = INVALID_SOCKET;
            client.done = true;
        });
    }
}

void Server::forget_old() {
    // Data for the newest `kept` streams not in use (the one about to be added counts), and every
    // stream in use; ids for `ids` streams.
    std::vector<std::pair<Clock::time_point, int>> by_use;
    for (const auto& [id, s] : streams_) {
        std::scoped_lock lock(s->m);
        by_use.emplace_back(s->used, id);
    }
    std::ranges::sort(by_use, std::greater<>{});
    for (std::size_t i = 0; i < by_use.size(); ++i) {
        const auto& s = streams_.at(by_use[i].second);
        std::scoped_lock lock(s->m);
        if (s->requests > 0) continue;
        if (i + 1 >= options_.kept && (s->running || !s->blocks.empty())) s->abandoned = true; // the keeper frees it
        if (i + 1 >= options_.ids) s->abandoned = s->forgotten = true;
    }
}

std::expected<std::string, std::string> Server::play(File file, const Progress& on_progress, std::stop_token stop) {
    if (!file.pool) return util::fail("no connection to the server");
    const auto started = Clock::now();
    // A seat first, at High priority: downloads above their share step aside.
    const int seat_id = connections_.new_id();
    connections_.join(file.server, seat_id, high);
    if (!connections_.acquire(file.server, seat_id, stop, false)) {
        connections_.leave(file.server, seat_id);
        return util::fail("stopped");
    }
    // Its size and kind from STAT, not the row (review G46): only a regular file is opened.
    std::uint64_t size = 0;
    {
        ssh::ReadOptions o;
        o.stop = stop;
        ssh::Reader reader(file.pool, file.path, std::move(o));
        auto st = reader.stat();
        std::optional<std::string> failed;
        if (!st) failed = why(st.error(), file.server);
        else if (!st->size) failed = std::format("{} didn't say how big the file is", file.server);
        if (failed) {
            connections_.release(file.server, seat_id);
            connections_.leave(file.server, seat_id);
            return util::fail(*failed);
        }
        size = *st->size;
    } // its connection goes back to the pool, where the first lane finds it

    auto s = std::make_shared<Stream>(*this, std::move(file), size, seat_id);
    {
        std::scoped_lock lock(mutex_);
        if (closing_) {
            connections_.release(s->file.server, seat_id);
            connections_.leave(s->file.server, seat_id);
            return util::fail("stopped");
        }
        if (listener_ == INVALID_SOCKET) {
            if (auto ok = listen(); !ok) {
                connections_.release(s->file.server, seat_id);
                connections_.leave(s->file.server, seat_id);
                return util::fail(ok.error());
            }
        }
        forget_old();
        s->id = next_id_++;
        streams_[s->id] = s;
    }
    kick(); // the keeper frees what forget_old let go

    std::unique_lock lock(s->m);
    s->joined = true;
    s->seat_users = 1;
    s->used = s->last_in = s->last_request = Clock::now();
    // The warm part: the start and the end at once. An MKV may hold tens of MB (fonts) before its
    // first video: its start is taken as 32 MiB, without waiting for the first bytes to say so (a
    // login takes seconds on a Storage Box; review G37); the first 64 KiB then decide.
    const std::uint64_t tail_at = size > preview::warm_first + preview::warm_tail ? size - preview::warm_tail : size;
    const bool mkv = s->type.ends_with("matroska") || s->type == "video/webm";
    s->plan.warm(mkv ? 2 * preview::warm_first : preview::warm_margin, tail_at);
    if (size == 0) {
        s->decided = true;
        s->plan.warm_done();
    }
    s->start_lanes(true);

    // The player starts once what it reads while opening is in (decision 3), or after
    // start_within while bytes still come.
    auto last_report = Clock::time_point{};
    std::optional<std::string> failed;
    for (;;) {
        const auto now = Clock::now();
        if (stop.stop_requested()) {
            failed = "stopped";
            break;
        }
        if (s->failure) {
            failed = *s->failure;
            break;
        }
        if (s->decided && !s->plan.warming()) break;
        if (now - started >= options_.start_within && now - s->last_in < options_.stalled_after) break;
        if (now - s->last_in >= options_.stalled_after) {
            failed = std::format("no data from {} for {} s", s->file.server,
                                 std::chrono::duration_cast<std::chrono::seconds>(options_.stalled_after).count());
            break;
        }
        if (on_progress && now - last_report >= std::chrono::milliseconds(200)) {
            last_report = now;
            const auto [done, total] = s->plan.warm_progress();
            lock.unlock();
            on_progress(done, total);
            lock.lock();
            continue;
        }
        s->cv.wait_for(lock, stop, std::chrono::milliseconds(200),
                       [&] { return s->failure.has_value() || (s->decided && !s->plan.warming()); });
    }
    if (failed) {
        s->abandoned = true; // the keeper stops its lanes and frees its data
        lock.unlock();
        kick();
        return util::fail(*failed);
    }
    s->last_request = s->used = Clock::now(); // the connections go back release_after from now if no player comes
    const auto [done, total] = s->plan.warm_progress();
    lock.unlock();
    if (on_progress) on_progress(done, total);
    return std::format("{}{}/{}", base(), s->id, preview::url_path(s->file.name));
}

void Server::serve(SOCKET c) {
    // The head, within head_within in all and max_head (review G49).
    std::string in;
    const auto deadline = Clock::now() + options_.head_within;
    std::size_t head_end = std::string::npos;
    while ((head_end = in.find("\r\n\r\n")) == std::string::npos && in.size() <= options_.max_head) {
        if (closing_) return;
        // A select already waiting isn't woken by ~Server: at most a short wait at a time.
        const auto left = std::min<std::int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(deadline - Clock::now()).count(), 250'000);
        if (left <= 0) return;
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(c, &readable);
        timeval tv{.tv_sec = static_cast<long>(left / 1'000'000), .tv_usec = static_cast<long>(left % 1'000'000)};
        const int ready = ::select(0, &readable, nullptr, nullptr, &tv);
        if (ready < 0) return;
        if (ready == 0) continue; // the deadline and closing_ again
        std::array<char, 4096> buf{};
        const int n = ::recv(c, buf.data(), static_cast<int>(buf.size()), 0);
        if (n <= 0) return;
        in.append(buf.data(), static_cast<std::size_t>(n));
    }
    if (head_end == std::string::npos || head_end + 4 > options_.max_head) {
        send_all(c, bare("431 Request Header Fields Too Large"));
        return;
    }
    const auto request = parse_request(std::string_view(in).substr(0, head_end + 4));
    if (!request) {
        send_all(c, bare("400 Bad Request"));
        return;
    }
    // This server only: a web page whose name now leads to 127.0.0.1 (DNS rebinding) sends its own.
    if (request->host != std::format("127.0.0.1:{}", port_)) {
        send_all(c, bare("403 Forbidden"));
        return;
    }
    // "/TOKEN/ID/name"
    std::string_view target = request->target;
    target.remove_prefix(1);
    const auto slash = target.find('/');
    if (slash == std::string_view::npos || !same_token(target.substr(0, slash), token_)) {
        send_all(c, bare("404 Not Found"));
        return;
    }
    target.remove_prefix(slash + 1);
    int id = 0;
    const auto [id_end, id_err] = std::from_chars(target.data(), target.data() + target.size(), id);
    std::shared_ptr<Stream> s;
    if (id_err == std::errc{} && (id_end == target.data() + target.size() || *id_end == '/')) {
        std::scoped_lock lock(mutex_);
        if (const auto it = streams_.find(id); it != streams_.end()) s = it->second;
    }
    if (s) {
        std::scoped_lock lock(s->m);
        if (s->forgotten) s.reset(); // on its way out
    }
    if (!s) {
        send_all(c, bare("404 Not Found"));
        return;
    }
    if (request->method != "GET" && request->method != "HEAD") {
        send_all(c, "HTTP/1.1 405 Method Not Allowed\r\nAllow: GET, HEAD\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        return;
    }
    const auto a = answer(*request, s->size, s->type);
    if (!send_all(c, a.head) || request->method == "HEAD" || a.length == 0) return;

    std::unique_lock lock(s->m);
    // Its data may have been freed, or a Play stopped: the lanes start again.
    s->cv.wait(lock, [&] { return !s->halting; });
    if (s->forgotten) return; // let go meanwhile: the player's next request gets 404
    s->failure.reset(); // a new request tries again
    s->breaks = 0;
    s->abandoned = false;
    if (!s->running) s->start_lanes(false);
    ++s->requests;
    s->used = Clock::now();
    const std::uint64_t last = a.first + a.length - 1;
    const int reader = s->plan.add_reader(a.first, last);
    s->cv.notify_all();
    std::uint64_t pos = a.first;
    while (pos <= last) {
        auto in_cache = [&] { return s->plan.have.until(pos) > pos; };
        const auto waiting_since = Clock::now();
        while (!in_cache()) {
            if (closing_ || s->failure || pos >= s->plan.end()) break;
            if (Clock::now() - std::max(waiting_since, s->last_in) >= options_.stalled_after) break;
            s->cv.wait_for(lock, std::chrono::milliseconds(250), in_cache);
        }
        if (!in_cache()) break; // a failure: the player's next request tries again
        const std::uint64_t n = std::min({s->plan.have.until(pos) - pos, last - pos + 1, std::uint64_t{send_piece}});
        const std::string piece = s->copy(pos, n);
        s->plan.move_reader(reader, pos + piece.size());
        // The blocks behind the request may go now: room for read-ahead again (no store() comes
        // to evict while the cache is full).
        if (!s->room()) s->evict();
        s->cv.notify_all(); // read-ahead moves on
        lock.unlock();
        const bool sent = !piece.empty() && send_all(c, piece);
        lock.lock();
        if (!sent) break;
        pos += piece.size();
    }
    s->plan.remove_reader(reader);
    --s->requests;
    s->last_request = s->used = Clock::now();
    s->cv.notify_all();
}

void Server::housekeep(const std::stop_token& stop) {
    for (;;) {
        std::vector<std::shared_ptr<Stream>> to_free;
        std::list<Client> finished;
        {
            std::unique_lock lock(mutex_);
            changed_.wait_for(lock, stop, options_.housekeeping, [this] { return std::exchange(kicked_, false); });
            if (stop.stop_requested()) return;
            for (auto it = clients_.begin(); it != clients_.end();) {
                if (it->done) finished.splice(finished.end(), clients_, it++);
                else ++it;
            }
            const auto now = Clock::now();
            for (auto it = streams_.begin(); it != streams_.end();) {
                const auto& s = it->second;
                std::scoped_lock stream_lock(s->m);
                const bool idle = s->requests == 0 && (!s->plan.warming() || s->failure) &&
                                  now - s->last_request >= options_.free_after;
                const bool in_use = s->requests > 0;
                if (!in_use && (s->abandoned || (idle && s->running))) to_free.push_back(s);
                if (!in_use && s->forgotten) {
                    it = streams_.erase(it);
                    continue;
                }
                ++it;
            }
        }
        finished.clear(); // their threads are done: joined at once
        // Idle data freed (review G141, G145): the lanes stop, the id stays for a later request.
        for (const auto& s : to_free) {
            std::vector<std::unique_ptr<Stream::Lane>> lanes;
            {
                std::scoped_lock lock(s->m);
                if (s->halting || s->requests > 0) continue;
                s->halting = true;
                lanes = s->take_lanes();
            }
            lanes.clear(); // joined: each drains or drops what it has on its way
            std::scoped_lock lock(s->m);
            s->blocks.clear();
            s->plan.have.clear();
            s->plan.taken.clear();
            s->running = false;
            s->halting = false;
            s->abandoned = false;
            s->cv.notify_all();
        }
    }
}

void Server::kick() {
    {
        std::scoped_lock lock(mutex_);
        kicked_ = true;
    }
    changed_.notify_all();
}

Server::Stats Server::stats() {
    Stats out;
    std::scoped_lock lock(mutex_);
    out.streams = static_cast<int>(streams_.size());
    out.clients = static_cast<int>(std::ranges::count_if(clients_, [](const Client& c) { return !c.done; }));
    for (const auto& [id, s] : streams_) {
        std::scoped_lock stream_lock(s->m);
        if (!s->blocks.empty()) ++out.with_data;
        out.cached += s->cached();
        out.ahead += s->ahead_taken;
        for (const auto& lane : s->lanes) out.connected += lane->connected ? 1 : 0;
        out.seats += s->joined ? connections_.active(s->file.server, s->seat_id) : 0;
    }
    return out;
}

} // namespace grab::play
