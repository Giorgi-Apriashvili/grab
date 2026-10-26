#include "sftp.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <utility>

namespace grab::sftp {

namespace {

enum : std::uint8_t {
    fxp_init = 1,
    fxp_version = 2,
    fxp_open = 3,
    fxp_close = 4,
    fxp_read = 5,
    fxp_lstat = 7,
    fxp_fstat = 8,
    fxp_opendir = 11,
    fxp_readdir = 12,
    fxp_realpath = 16,
    fxp_stat = 17,
    fxp_status = 101,
    fxp_handle = 102,
    fxp_data = 103,
    fxp_name = 104,
    fxp_attrs = 105,
    fxp_extended_reply = 201,
};

constexpr std::uint32_t open_read = 0x1;
constexpr std::uint32_t attr_size = 0x1, attr_uidgid = 0x2, attr_permissions = 0x4, attr_acmodtime = 0x8,
                        attr_extended = 0x80000000;
// The largest packet taken from a server: a 255 KiB DATA plus headers, or a NAME batch (OpenSSH
// sends at most 256 KiB); anything far bigger is a broken or hostile server (review G168).
constexpr std::uint32_t max_packet = 1024 * 1024;

void put_u8(std::string& out, std::uint8_t v) { out.push_back(static_cast<char>(v)); }
void put_u32(std::string& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<char>((v >> shift) & 0xFF));
}
void put_u64(std::string& out, std::uint64_t v) {
    put_u32(out, static_cast<std::uint32_t>(v >> 32));
    put_u32(out, static_cast<std::uint32_t>(v));
}
void put_str(std::string& out, std::string_view s) {
    put_u32(out, static_cast<std::uint32_t>(s.size()));
    out.append(s);
}

// Reads a reply's fields; running off its end means the server broke the protocol.
class Fields {
public:
    explicit Fields(std::string_view body) : rest_(body) {}
    [[nodiscard]] bool empty() const { return rest_.empty(); }
    std::uint32_t u32() {
        need(4);
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | static_cast<std::uint8_t>(rest_[static_cast<std::size_t>(i)]);
        rest_.remove_prefix(4);
        return v;
    }
    std::uint64_t u64() {
        std::uint64_t high = u32();
        return (high << 32) | u32();
    }
    std::string_view str() {
        std::uint32_t n = u32();
        need(n);
        std::string_view s = rest_.substr(0, n);
        rest_.remove_prefix(n);
        return s;
    }

private:
    void need(std::size_t n) const {
        if (rest_.size() < n) throw Broken("the server sent a malformed SFTP reply");
    }
    std::string_view rest_;
};

Attrs read_attrs(Fields& f) {
    Attrs a;
    std::uint32_t flags = f.u32();
    if (flags & attr_size) a.size = f.u64();
    if (flags & attr_uidgid) {
        f.u32();
        f.u32();
    }
    if (flags & attr_permissions) a.permissions = f.u32();
    if (flags & attr_acmodtime) {
        f.u32(); // atime
        a.mtime = f.u32();
    }
    if (flags & attr_extended) {
        std::uint32_t n = f.u32();
        for (std::uint32_t i = 0; i < n; ++i) {
            f.str();
            f.str();
        }
    }
    return a;
}

Status read_status(std::string_view body) {
    Fields f(body);
    Status s;
    s.code = static_cast<Code>(f.u32());
    if (!f.empty()) s.message = f.str(); // version 3 servers send it; some old ones don't
    return s;
}

[[noreturn]] void unexpected_reply(std::uint8_t type) {
    throw Broken(std::format("the server answered with an unexpected SFTP packet ({})", type));
}

} // namespace

std::string describe(const Status& status) {
    switch (status.code) {
    case Code::no_such_file: return "no such file or folder";
    case Code::permission_denied: return "permission denied";
    case Code::eof: return "the file ended early";
    case Code::op_unsupported: return "the server doesn't support this";
    case Code::no_connection:
    case Code::connection_lost: return "the connection was lost";
    default:
        if (!status.message.empty()) return status.message;
        return std::format("the server refused (SFTP status {})", static_cast<std::uint32_t>(status.code));
    }
}

bool Attrs::is_dir() const { return permissions && (*permissions & 0170000) == 0040000; }
bool Attrs::is_file() const { return permissions && (*permissions & 0170000) == 0100000; }
bool Attrs::is_link() const { return permissions && (*permissions & 0170000) == 0120000; }

Client::Client(Channel& channel) : channel_(channel), in_(std::size_t{256} * 1024) {}

void Client::start() {
    put_u32(out_, 5);
    put_u8(out_, fxp_init);
    put_u32(out_, 3);
    flush();
    read_packet();
    if (static_cast<std::uint8_t>(packet_[0]) != fxp_version) throw Broken("the server didn't start SFTP");
    Fields f(std::string_view(packet_).substr(1));
    version_ = f.u32();
    if (version_ < 3) throw Broken(std::format("the server speaks SFTP version {}; grab needs 3", version_));
    while (!f.empty()) {
        extensions_.emplace_back(f.str());
        f.str(); // its data
    }
}

bool Client::has_extension(std::string_view name) const {
    return std::ranges::find(extensions_, name) != extensions_.end();
}

void Client::request(std::uint8_t type, std::string_view payload, Handler handler) {
    std::uint32_t id = next_id_++;
    put_u32(out_, static_cast<std::uint32_t>(1 + 4 + payload.size()));
    put_u8(out_, type);
    put_u32(out_, id);
    out_.append(payload);
    pending_.emplace(id, std::move(handler));
}

void Client::flush() {
    if (out_.empty()) return;
    channel_.write(out_);
    out_.clear();
}

void Client::pump() {
    if (pending_.empty()) throw Broken("waiting for an SFTP reply with no request pending");
    flush();
    read_packet();
    auto type = static_cast<std::uint8_t>(packet_[0]);
    if (type != fxp_status && type != fxp_handle && type != fxp_data && type != fxp_name && type != fxp_attrs &&
        type != fxp_extended_reply)
        unexpected_reply(type);
    Fields f(std::string_view(packet_).substr(1));
    std::uint32_t id = f.u32();
    auto it = pending_.find(id);
    if (it == pending_.end()) throw Broken("the server answered a request grab didn't send");
    Handler handler = std::move(it->second);
    pending_.erase(it);
    handler(Reply{type, std::string_view(packet_).substr(5)});
}

void Client::read_packet() {
    char header[4];
    read_exact(header, 4);
    std::uint32_t length = 0;
    for (char c : header) length = (length << 8) | static_cast<std::uint8_t>(c);
    if (length < 1 || length > max_packet)
        throw Broken(std::format("the server sent an SFTP packet of {} bytes", length));
    packet_.resize(length);
    read_exact(packet_.data(), length);
}

void Client::read_exact(char* to, std::size_t n) {
    while (n > 0) {
        if (in_pos_ == in_len_) {
            in_len_ = channel_.read(in_);
            in_pos_ = 0;
        }
        std::size_t take = std::min(n, in_len_ - in_pos_);
        std::memcpy(to, in_.data() + in_pos_, take);
        in_pos_ += take;
        to += take;
        n -= take;
    }
}

// Each request below wraps its callback in a handler that parses the reply it expects.

void Client::open(std::string_view path, Done<std::string> done) {
    std::string p;
    put_str(p, path);
    put_u32(p, open_read);
    put_u32(p, 0); // no attributes
    request(fxp_open, p, [done = std::move(done)](const Reply& r) mutable {
        if (r.type == fxp_status) return done(std::unexpected(read_status(r.body)));
        if (r.type != fxp_handle) unexpected_reply(r.type);
        Fields f(r.body);
        done(std::string(f.str()));
    });
}

void Client::opendir(std::string_view path, Done<std::string> done) {
    std::string p;
    put_str(p, path);
    request(fxp_opendir, p, [done = std::move(done)](const Reply& r) mutable {
        if (r.type == fxp_status) return done(std::unexpected(read_status(r.body)));
        if (r.type != fxp_handle) unexpected_reply(r.type);
        Fields f(r.body);
        done(std::string(f.str()));
    });
}

void Client::read(std::string_view handle, std::uint64_t offset, std::uint32_t length, DataDone done) {
    std::string p;
    put_str(p, handle);
    put_u64(p, offset);
    put_u32(p, length);
    request(fxp_read, p, [done = std::move(done), length](const Reply& r) mutable {
        if (r.type == fxp_status) {
            Status s = read_status(r.body);
            if (s.code == Code::ok) throw Broken("the server answered a READ with no data");
            return done(std::unexpected(std::move(s)));
        }
        if (r.type != fxp_data) unexpected_reply(r.type);
        Fields f(r.body);
        std::string_view data = f.str();
        if (data.empty() || data.size() > length) throw Broken("the server sent more data than asked for, or none");
        done(data);
    });
}

void Client::close(std::string_view handle, Done<void> done) {
    std::string p;
    put_str(p, handle);
    request(fxp_close, p, [done = std::move(done)](const Reply& r) mutable {
        if (r.type != fxp_status) unexpected_reply(r.type);
        Status s = read_status(r.body);
        if (!done) return;
        if (s.code == Code::ok) return done({});
        done(std::unexpected(std::move(s)));
    });
}

void Client::readdir(std::string_view handle, Done<std::vector<Entry>> done) {
    std::string p;
    put_str(p, handle);
    request(fxp_readdir, p, [done = std::move(done)](const Reply& r) mutable {
        if (r.type == fxp_status) return done(std::unexpected(read_status(r.body)));
        if (r.type != fxp_name) unexpected_reply(r.type);
        Fields f(r.body);
        std::uint32_t n = f.u32();
        std::vector<Entry> entries;
        entries.reserve(std::min<std::uint32_t>(n, 4096));
        for (std::uint32_t i = 0; i < n; ++i) {
            Entry e;
            e.name = f.str();
            f.str(); // longname, the `ls -l` line
            e.attrs = read_attrs(f);
            entries.push_back(std::move(e));
        }
        done(std::move(entries));
    });
}

void Client::attrs_request(std::uint8_t type, std::string_view arg, Done<Attrs> done) {
    std::string p;
    put_str(p, arg);
    request(type, p, [done = std::move(done)](const Reply& r) mutable {
        if (r.type == fxp_status) return done(std::unexpected(read_status(r.body)));
        if (r.type != fxp_attrs) unexpected_reply(r.type);
        Fields f(r.body);
        done(read_attrs(f));
    });
}

void Client::stat(std::string_view path, Done<Attrs> done) { attrs_request(fxp_stat, path, std::move(done)); }
void Client::lstat(std::string_view path, Done<Attrs> done) { attrs_request(fxp_lstat, path, std::move(done)); }
void Client::fstat(std::string_view handle, Done<Attrs> done) { attrs_request(fxp_fstat, handle, std::move(done)); }

void Client::realpath(std::string_view path, Done<std::string> done) {
    std::string p;
    put_str(p, path);
    request(fxp_realpath, p, [done = std::move(done)](const Reply& r) mutable {
        if (r.type == fxp_status) return done(std::unexpected(read_status(r.body)));
        if (r.type != fxp_name) unexpected_reply(r.type);
        Fields f(r.body);
        if (f.u32() < 1) throw Broken("the server answered REALPATH with no name");
        done(std::string(f.str()));
    });
}

namespace {

// Sends one request through `start` and pumps until its reply came.
template <typename T, typename Start> std::expected<T, Status> wait_for(Client& client, Start start) {
    std::optional<std::expected<T, Status>> result;
    start([&result](std::expected<T, Status> r) { result = std::move(r); });
    while (!result) client.pump();
    return std::move(*result);
}

} // namespace

std::expected<Attrs, Status> stat(Client& client, std::string_view path) {
    return wait_for<Attrs>(client, [&](Client::Done<Attrs> done) { client.stat(path, std::move(done)); });
}

std::expected<std::string, Status> realpath(Client& client, std::string_view path) {
    return wait_for<std::string>(client, [&](Client::Done<std::string> done) { client.realpath(path, std::move(done)); });
}

std::expected<std::vector<Entry>, Status> list(Client& client, std::string_view dir) {
    auto handle =
        wait_for<std::string>(client, [&](Client::Done<std::string> done) { client.opendir(dir, std::move(done)); });
    if (!handle) return std::unexpected(handle.error());
    std::vector<Entry> all;
    std::optional<Status> failed;
    for (;;) {
        auto batch = wait_for<std::vector<Entry>>(
            client, [&](Client::Done<std::vector<Entry>> done) { client.readdir(*handle, std::move(done)); });
        if (!batch) {
            if (batch.error().code != Code::eof) failed = batch.error();
            break;
        }
        for (Entry& e : *batch)
            if (e.name != "." && e.name != "..") all.push_back(std::move(e));
    }
    // A handle that fails to close doesn't spoil a listing that was read in full.
    [[maybe_unused]] auto closed =
        wait_for<void>(client, [&](Client::Done<void> done) { client.close(*handle, std::move(done)); });
    if (failed) return std::unexpected(std::move(*failed));
    return all;
}

std::expected<void, Status> read_range(Client& client, std::string_view handle, std::uint64_t offset,
                                       std::uint64_t length, const Sink& sink, int depth, std::uint32_t chunk) {
    const std::uint64_t end = offset + length;
    std::uint64_t next = offset;
    int in_flight = 0;
    std::optional<Status> failed;
    std::function<void(std::uint64_t, std::uint32_t)> send = [&](std::uint64_t at, std::uint32_t n) {
        ++in_flight;
        client.read(handle, at, n, [&, at, n](std::expected<std::string_view, Status> r) {
            --in_flight;
            if (!r) {
                if (!failed) failed = std::move(r.error());
                return;
            }
            if (failed) return;
            sink(at, *r);
            auto got = static_cast<std::uint32_t>(r->size());
            if (got < n) send(at + got, n - got); // short read: ask for exactly the rest
        });
    };
    chunk = std::clamp<std::uint32_t>(chunk, 1, max_read);
    depth = std::max(depth, 1);
    for (;;) {
        while (!failed && in_flight < depth && next < end) {
            auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(chunk, end - next));
            send(next, n);
            next += n;
        }
        if (in_flight == 0) break;
        client.pump();
    }
    if (failed) return std::unexpected(std::move(*failed));
    return {};
}

} // namespace grab::sftp
