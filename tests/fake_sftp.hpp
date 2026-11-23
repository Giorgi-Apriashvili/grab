#pragma once

// An SFTP server in memory for the tests (test_sftp.cpp, test_fetch_grab.cpp, test_upload.cpp).

#include "sftp.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace grab::test {

// A fake server's files and folders: a session's own, or one store several sessions share
// (FakeServer::shared), as one server's file system is shared by its logins. A file's bytes stay
// where they are while it is renamed, and an open handle keeps them when its name is removed or
// replaced, as an inode does: WRITE, READ and FSTAT on it go on, and the name doesn't come back.
struct FakeFiles {
    std::map<std::string, std::string> files;             // path -> content
    std::map<std::string, std::vector<std::string>> dirs; // path -> entry names (". and .." too)
    std::map<std::string, std::uint32_t> mtimes;          // times set (SETSTAT); else 1700000000
    std::map<std::string, std::uint32_t> modes;           // permissions given (OPEN, MKDIR, SETSTAT), without the type bits
    std::mutex mutex;                                     // held for each packet of a session sharing it

    // The bytes all named files hold, as the quota counts them.
    [[nodiscard]] std::uint64_t used() const {
        std::uint64_t n = 0;
        for (const auto& [path, content] : files) n += content.size();
        return n;
    }
    // The file's name goes; its bytes stay while a handle has them open.
    void unlink(const std::string& path) {
        auto node = files.extract(path);
        mtimes.erase(path);
        modes.erase(path);
        if (!node.empty() && open.contains(&node.mapped())) unlinked.push_back(std::move(node));
    }
    void opened(const std::string* content) { ++open[content]; }
    void closed(const std::string* content) {
        const auto it = open.find(content);
        if (it == open.end() || --it->second > 0) return;
        open.erase(it);
        std::erase_if(unlinked, [&](const auto& node) { return &node.mapped() == content; });
    }
    // The name a file's bytes have now (renamed meanwhile, perhaps by another session); empty once
    // it has none.
    [[nodiscard]] std::string path_of(const std::string* content) const {
        for (const auto& [path, bytes] : files)
            if (&bytes == content) return path;
        return {};
    }

private:
    std::map<const std::string*, int> open; // handles open on each file's bytes
    std::vector<std::map<std::string, std::string>::node_type> unlinked;
};

// An SFTP server in memory, answering the client's packets as they are written. Its switches
// reorder the replies, shorten READs, cut the stream into small pieces, refuse paths or corrupt
// the stream; it records the READs it was asked for.
//
// Writes follow OpenSSH's sftp-server: OPEN's flags as open(2) takes them (creat | excl fails on an
// existing name, trunc empties, append writes at the end), RENAME never replaces anything, MKDIR
// on an existing name and RMDIR of a folder with something in it fail with "Failure", and a name
// whose folder isn't there is "No such file". A new file or folder needs its folder in `dirs`.
class FakeServer : public sftp::Channel {
    FakeFiles own_; // the session's own files, unless `shared`

public:
    FakeServer() = default;
    FakeServer(const FakeServer&) = delete;
    FakeServer& operator=(const FakeServer&) = delete;
    ~FakeServer() override {
        // Its handles close with the session.
        const auto lock = locked();
        for (const auto& [h, open_handle] : handles_)
            if (open_handle.content != nullptr) st().closed(open_handle.content);
    }

    // The session's own files and folders (FakeFiles); a store several sessions write instead:
    // `shared`, every request of this session going through it under its lock.
    std::map<std::string, std::string>& files = own_.files;
    std::map<std::string, std::vector<std::string>>& dirs = own_.dirs;
    std::map<std::string, std::uint32_t>& mtimes = own_.mtimes;
    std::map<std::string, std::uint32_t>& modes = own_.modes;
    FakeFiles* shared = nullptr;
    std::map<std::string, std::uint64_t> listed_size;       // sizes to claim in ATTRS instead
    std::map<std::string, std::uint64_t> readdir_size;      // sizes to claim in folder listings only (a file changed since)
    std::map<std::string, std::string> links;               // path -> where the link points (a full path)
    std::set<std::string> denied;                           // answered with permission denied
    std::uint32_t max_data = sftp::max_read;                // a DATA carries at most this
    bool reverse = false;                                   // answer each batch last first
    std::size_t slice = 0;                                  // hand out at most this many bytes per read
    std::size_t readdir_batch = 2;                          // entries per NAME
    bool corrupt = false;                                   // send a packet claiming 1 GiB
    // Instead of files, for reading: one copy for many sessions. Read only: writes (OPEN for
    // writing, WRITE, SETSTAT, REMOVE, RENAME, MKDIR) go to the session's files (or `shared`'s).
    const std::map<std::string, std::string>* shared_files = nullptr;
    const std::map<std::string, std::uint32_t>* shared_mtimes = nullptr; // files' times instead of 1700000000
    std::chrono::milliseconds delay{0};                     // a pause before each read, as a network's
    std::chrono::milliseconds latency{0};                   // each reply comes this long after its request (a round trip)
    // The client's send of a WRITE takes this long, as one that waits for a full SSH window does on
    // a real link, the replies to earlier ones coming meanwhile: its WRITEs in flight never reach
    // their caps (with no latency).
    std::chrono::milliseconds write_pace{0};
    bool held = false;                                      // replies come only when the client waits for one (then all of them)
    bool release_one = false;                               // with held: each read hands over one reply
    int broken_after = 0;                                   // the stream breaks on this read (0: never)
    bool dropped = false;                                   // the server closed the connection: every read breaks
    bool break_on_close = false;                            // the connection breaks at a CLOSE, before its reply
    bool never_eof = false;                                 // past the last name, READDIR gets empty NAMEs, not EOF
    std::set<std::string> fifos;                            // named pipes: STAT says so (an OPEN would block a real server)
    std::set<std::string> no_size;                          // files whose ATTRS carry no size

    // Writes. Every change is refused while `read_only`: with `read_only_as` (permission denied, a
    // folder this account can't write, as Main's chmod 555 folder answers; or failure, "Failure",
    // as OpenSSH answers a read-only file system's EROFS and a Storage Box an account that may only
    // read, Box.Guest, whose statvfs then says ST_RDONLY).
    bool read_only = false;
    sftp::Code read_only_as = sftp::Code::permission_denied;
    std::uint64_t quota_bytes = 0;  // 0: none; else what all files may hold: a WRITE past it fails ("Failure", ENOSPC)
    bool posix_rename = true;       // posix-rename@openssh.com offered (and answered)
    bool rename_overwrites = false; // RENAME replaces an existing file, as a server unlike OpenSSH's might
    std::uint32_t max_write = 0;    // nonzero: limits@openssh.com offered, saying so; a bigger WRITE ends the session
    bool statvfs = false;           // statvfs@openssh.com offered: quota_bytes' figures (else 1 TiB), ST_RDONLY as above
    bool fsync = false;             // fsync@openssh.com offered
    bool no_setstat = false;        // SETSTAT and FSETSTAT answer "Operation unsupported", as some gateways do
    // Folders this account can't write in (chmod 555, as on Main): a new file or folder in one, a
    // REMOVE or RENAME of a name in one, answer "Permission denied".
    std::set<std::string> locked_dirs;
    // What MKDIR of a name there already answers: OpenSSH's "Failure", or "Permission denied" as a
    // server that refuses MKDIR without looking (SFTPGo without create_dirs, ProFTPD's <Limit MKD>).
    sftp::Code mkdir_existing_as = sftp::Code::failure;
    std::set<std::string> rename_refused; // a plain RENAME to one of these answers "Permission denied"
    // Set: the next RENAME or posix-rename done (by any session pointing here) breaks its
    // connection before the reply goes; cleared by it.
    std::atomic<bool>* break_after_rename = nullptr;
    std::optional<std::string> bad_extended_reply; // every EXTENDED gets an EXTENDED_REPLY of just these bytes

    std::vector<std::pair<std::uint64_t, std::uint32_t>> reads; // (offset, length) of each READ
    std::vector<std::string> opened;                            // the paths of each OPEN
    // Each WRITE as it came, with the WRITEs (and their bytes) answered not yet, itself included:
    // the client had at least that many in flight (replies handed over may not be taken yet).
    struct Write {
        std::string path;
        std::uint64_t offset;
        std::uint32_t length;
        std::size_t out;
        std::uint64_t out_bytes;
    };
    std::vector<Write> writes;
    std::vector<std::string> requests;                          // the type of each request after INIT, by name
    std::function<void(const std::string&)> on_open;            // told each OPEN's path as it comes
    std::function<void(const std::string&)> on_close;           // told each CLOSE's path (a file's or folder's) as it comes
    std::function<void(const std::string&)> on_stat;            // told each STAT's and LSTAT's path before it is answered
    std::function<void(const std::string&, std::uint32_t)> on_read; // told each READ's path and length
    std::size_t most_waiting = 0;                               // most requests answered in one batch
    std::uint64_t most_read_bytes = 0;                          // most bytes of DATA answered in one batch

    void write(std::string_view bytes) override {
        in_ += bytes;
        int paced = 0; // WRITEs in these bytes
        while (in_.size() >= 4) {
            std::uint32_t len = be32(in_.data());
            if (in_.size() < 4 + len) break;
            if (len > 0 && static_cast<std::uint8_t>(in_[4]) == 6) ++paced; // WRITE
            {
                const auto lock = locked();
                handle(std::string_view(in_).substr(4, len));
            }
            in_.erase(0, 4 + len);
        }
        if (write_pace.count() > 0 && paced > 0) std::this_thread::sleep_for(write_pace * paced);
    }

    std::size_t read(std::span<char> buffer) override {
        if (delay.count() > 0) std::this_thread::sleep_for(delay);
        if (broken_after > 0 && ++read_calls_ >= broken_after) throw sftp::Broken("fake server: the connection broke");
        if (dropped) throw sftp::Broken("fake server: the server closed the connection");
        if (out_.empty()) {
            // The replies due by now (all of them without a latency, or held until now; one with
            // release_one): the first waited for.
            std::size_t due = held && release_one ? std::min<std::size_t>(1, queue_.size()) : queue_.size();
            if (latency.count() > 0 && !held && !queue_.empty()) {
                std::this_thread::sleep_until(due_.front());
                const auto now = std::chrono::steady_clock::now();
                due = 0;
                while (due < due_.size() && due_[due] <= now) ++due;
            }
            std::vector<std::string> batch(std::make_move_iterator(queue_.begin()),
                                           std::make_move_iterator(queue_.begin() + static_cast<std::ptrdiff_t>(due)));
            queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(due));
            due_.erase(due_.begin(), due_.begin() + static_cast<std::ptrdiff_t>(due));
            for (std::size_t i = 0; i < due; ++i) {
                if (!write_of_[i]) continue;
                --writes_out_;
                write_bytes_out_ -= *write_of_[i];
            }
            write_of_.erase(write_of_.begin(), write_of_.begin() + static_cast<std::ptrdiff_t>(due));
            most_waiting = std::max(most_waiting, batch.size());
            std::uint64_t data = 0;
            for (const std::string& r : batch) {
                if (r.size() >= 13 && static_cast<std::uint8_t>(r[4]) == 103) data += r.size() - 13; // DATA: its bytes
            }
            most_read_bytes = std::max(most_read_bytes, data);
            if (reverse) std::ranges::reverse(batch);
            for (const std::string& r : batch) out_ += r;
        }
        if (corrupt) out_ = std::string("\x40\x00\x00\x00", 4) + "x";
        if (out_.empty()) throw sftp::Broken("fake server: nothing to send");
        std::size_t n = std::min(buffer.size(), out_.size());
        if (slice) n = std::min(n, slice);
        std::memcpy(buffer.data(), out_.data(), n);
        out_.erase(0, n);
        return n;
    }

    // A read would answer at once: replies are due (or it breaks).
    bool readable() override {
        if (dropped || corrupt || !out_.empty()) return true;
        if (queue_.empty() || held) return false;
        return latency.count() == 0 || due_.front() <= std::chrono::steady_clock::now();
    }

    // The bytes all files hold, as the quota counts them.
    [[nodiscard]] std::uint64_t used() {
        const auto lock = locked();
        return st().used();
    }

private:
    [[nodiscard]] FakeFiles& st() { return shared != nullptr ? *shared : own_; }
    [[nodiscard]] const FakeFiles& st() const { return shared != nullptr ? *shared : own_; }
    // The shared store's lock, for a packet; none for the session's own files.
    std::unique_lock<std::mutex> locked() {
        return shared != nullptr ? std::unique_lock(shared->mutex) : std::unique_lock<std::mutex>();
    }
    static std::uint32_t be32(const char* p) {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | static_cast<std::uint8_t>(p[i]);
        return v;
    }
    struct In {
        std::string_view s;
        std::uint32_t u32() {
            std::uint32_t v = be32(s.data());
            s.remove_prefix(4);
            return v;
        }
        std::uint64_t u64() {
            std::uint64_t h = u32();
            return (h << 32) | u32();
        }
        std::string str() {
            std::uint32_t n = u32();
            std::string v(s.substr(0, n));
            s.remove_prefix(n);
            return v;
        }
        // ATTRS as a client sends them (SETSTAT, OPEN, MKDIR).
        sftp::Attrs attrs() {
            sftp::Attrs a;
            const std::uint32_t flags = u32();
            if (flags & 0x1) a.size = u64();
            if (flags & 0x2) {
                u32();
                u32();
            }
            if (flags & 0x4) a.permissions = u32();
            if (flags & 0x8) {
                u32(); // atime
                a.mtime = u32();
            }
            if (flags & 0x80000000) {
                const std::uint32_t n = u32();
                for (std::uint32_t i = 0; i < n; ++i) {
                    str();
                    str();
                }
            }
            return a;
        }
    };
    static void u8(std::string& o, std::uint8_t v) { o.push_back(static_cast<char>(v)); }
    static void u32(std::string& o, std::uint32_t v) {
        for (int sh = 24; sh >= 0; sh -= 8) o.push_back(static_cast<char>((v >> sh) & 0xFF));
    }
    static void u64(std::string& o, std::uint64_t v) {
        u32(o, static_cast<std::uint32_t>(v >> 32));
        u32(o, static_cast<std::uint32_t>(v));
    }
    static void str(std::string& o, std::string_view v) {
        u32(o, static_cast<std::uint32_t>(v.size()));
        o.append(v);
    }
    void push(std::string r) {
        queue_.push_back(std::move(r));
        due_.push_back(std::chrono::steady_clock::now() + latency);
        write_of_.emplace_back();
    }
    void reply(std::uint8_t type, std::uint32_t id, const std::string& body) {
        std::string r;
        u32(r, static_cast<std::uint32_t>(1 + 4 + body.size()));
        u8(r, type);
        u32(r, id);
        r += body;
        push(std::move(r));
    }
    void status(std::uint32_t id, std::uint32_t code, std::string_view msg = "") {
        std::string b;
        u32(b, code);
        str(b, msg);
        str(b, "en");
        reply(101, id, b);
    }
    // The refusal of a change while read_only.
    void refuse(std::uint32_t id) {
        status(id, static_cast<std::uint32_t>(read_only_as),
               read_only_as == sftp::Code::permission_denied ? "Permission denied" : "Failure");
    }
    // Where a path leads through links (a few levels; a loop stays on the link).
    std::string resolve(std::string path) const {
        for (int i = 0; i < 8 && links.contains(path); ++i) path = links.at(path);
        return path;
    }
    void link_attrs(std::string& b) {
        u32(b, 0x4 | 0x8);
        u32(b, 0120777);
        u32(b, 0);
        u32(b, 1700000000);
    }
    [[nodiscard]] std::uint32_t mtime_of(const std::string& path) const {
        if (st().mtimes.contains(path)) return st().mtimes.at(path);
        return shared_mtimes != nullptr && shared_mtimes->contains(path) ? shared_mtimes->at(path) : 1700000000;
    }
    void attrs(std::string& b, const std::string& path, bool in_listing = false) {
        if (fifos.contains(path)) {
            u32(b, 0x1 | 0x4 | 0x8);
            u64(b, 0);
            u32(b, 0010644);
            u32(b, 0);
            u32(b, 1700000000);
            return;
        }
        if (no_size.contains(path)) {
            u32(b, 0x4 | 0x8);
            u32(b, 0100644);
            u32(b, 0);
            u32(b, 1700000000);
            return;
        }
        const FakeFiles& fs = st();
        if (fs.dirs.contains(path)) {
            u32(b, 0x4 | 0x8);
            u32(b, 040000 | (fs.modes.contains(path) ? fs.modes.at(path) : 0755));
            u32(b, 0);
            u32(b, mtime_of(path));
            return;
        }
        u32(b, 0x1 | 0x4 | 0x8);
        u64(b, in_listing && readdir_size.contains(path) ? readdir_size[path] : listed_size.contains(path) ? listed_size[path] : all_files().at(path).size());
        u32(b, 0100000 | (fs.modes.contains(path) ? fs.modes.at(path) : 0644));
        u32(b, mtime_of(path));
        u32(b, mtime_of(path));
    }
    [[nodiscard]] const std::map<std::string, std::string>& all_files() const { return shared_files ? *shared_files : st().files; }
    [[nodiscard]] bool known(const std::string& path) const {
        return all_files().contains(path) || st().dirs.contains(path) || fifos.contains(path);
    }
    // `content`: the bytes of a file of the store open on it (writes go there, as do READ and FSTAT).
    std::string new_handle(std::string path, bool writable = false, bool append = false, std::string* content = nullptr) {
        std::string h = "h" + std::to_string(next_handle_++);
        if (content != nullptr) st().opened(content);
        handles_[h] = {.path = std::move(path), .pos = 0, .writable = writable, .append = append, .content = content};
        return h;
    }
    // The folder a path is in ("/" for "/x"); empty for a name without one.
    static std::string parent_of(const std::string& path) {
        const auto slash = path.rfind('/');
        if (slash == std::string::npos) return {};
        return slash == 0 ? "/" : path.substr(0, slash);
    }
    static std::string name_of(const std::string& path) {
        const auto slash = path.rfind('/');
        return slash == std::string::npos ? path : path.substr(slash + 1);
    }
    // A new name's folder: there (or the name has none).
    [[nodiscard]] bool parent_there(const std::string& path) const {
        const std::string parent = parent_of(path);
        return parent.empty() || st().dirs.contains(parent);
    }
    void listed(const std::string& path) {
        const std::string parent = parent_of(path);
        if (!parent.empty() && st().dirs.contains(parent)) st().dirs[parent].push_back(name_of(path));
    }
    void unlisted(const std::string& path) {
        const std::string parent = parent_of(path);
        if (!parent.empty() && st().dirs.contains(parent)) std::erase(st().dirs[parent], name_of(path));
    }
    // Every path at or below `from` moves below `to` (a folder renamed), this session's open handles
    // too (a file's bytes move with it, so other sessions' handles keep writing into it).
    void move_tree(const std::string& from, const std::string& to) {
        const auto moved = [&](const std::string& p) -> std::optional<std::string> {
            if (p == from) return to;
            if (p.starts_with(from + "/")) return to + p.substr(from.size());
            return std::nullopt;
        };
        const auto move_keys = [&](auto& map) {
            std::vector<std::string> keys;
            for (const auto& [k, v] : map)
                if (moved(k)) keys.push_back(k);
            for (const std::string& k : keys) {
                auto node = map.extract(k);
                node.key() = *moved(k);
                map.insert(std::move(node));
            }
        };
        FakeFiles& fs = st();
        move_keys(fs.files);
        move_keys(fs.dirs);
        move_keys(fs.mtimes);
        move_keys(fs.modes);
        for (auto& [h, open_handle] : handles_)
            if (auto p = moved(open_handle.path)) open_handle.path = *p;
    }
    [[nodiscard]] bool empty_dir(const std::string& path) const {
        return std::ranges::all_of(st().dirs.at(path), [](const std::string& n) { return n == "." || n == ".."; });
    }

    void handle(std::string_view packet) {
        auto type = static_cast<std::uint8_t>(packet[0]);
        In in{packet.substr(1)};
        FakeFiles& fs = st();
        if (type == 1) { // INIT: no id
            std::string r;
            std::string b;
            u32(b, 3);
            if (posix_rename) {
                str(b, "posix-rename@openssh.com");
                str(b, "1");
            }
            if (statvfs) {
                str(b, "statvfs@openssh.com");
                str(b, "2");
            }
            if (fsync) {
                str(b, "fsync@openssh.com");
                str(b, "1");
            }
            if (max_write != 0) {
                str(b, "limits@openssh.com");
                str(b, "1");
            }
            u32(r, static_cast<std::uint32_t>(1 + b.size()));
            u8(r, 2);
            r += b;
            push(std::move(r));
            return;
        }
        std::uint32_t id = in.u32();
        static const std::map<std::uint8_t, std::string> type_names{
            {3, "OPEN"},     {4, "CLOSE"},   {5, "READ"},  {6, "WRITE"},     {7, "LSTAT"},     {8, "FSTAT"},
            {9, "SETSTAT"},  {10, "FSETSTAT"}, {11, "OPENDIR"}, {12, "READDIR"}, {13, "REMOVE"}, {14, "MKDIR"},
            {15, "RMDIR"},   {16, "REALPATH"}, {17, "STAT"}, {18, "RENAME"},   {200, "EXTENDED"}};
        requests.push_back(type_names.contains(type) ? type_names.at(type) : std::to_string(type));
        switch (type) {
        case 3: { // OPEN
            std::string path = resolve(in.str());
            const std::uint32_t flags = in.u32();
            const sftp::Attrs given = in.attrs();
            opened.push_back(path);
            if (on_open) on_open(path);
            if (denied.contains(path)) return status(id, 3, "Permission denied");
            using F = sftp::OpenFlags;
            if ((flags & (F::write | F::append | F::creat | F::trunc)) == 0) {
                if (!all_files().contains(path)) return status(id, 2, "No such file");
                std::string b;
                str(b, new_handle(path, false, false, shared_files == nullptr ? &fs.files.at(path) : nullptr));
                return reply(102, id, b);
            }
            if (read_only) return refuse(id);
            if (fs.dirs.contains(path) || fifos.contains(path)) return status(id, 4, "Failure"); // EISDIR
            if (fs.files.contains(path)) {
                if ((flags & F::creat) && (flags & F::excl)) return status(id, 4, "Failure"); // EEXIST
                if (flags & F::trunc) fs.files[path].clear();
            } else {
                if (!(flags & F::creat) || !parent_there(path)) return status(id, 2, "No such file");
                if (locked_dirs.contains(parent_of(path))) return status(id, 3, "Permission denied");
                fs.files[path] = {};
                listed(path);
                if (given.permissions) fs.modes[path] = *given.permissions & 07777;
            }
            std::string b;
            str(b, new_handle(path, (flags & (F::write | F::append)) != 0, (flags & F::append) != 0, &fs.files.at(path)));
            return reply(102, id, b);
        }
        case 11: { // OPENDIR
            std::string path = resolve(in.str());
            if (denied.contains(path)) return status(id, 3, "Permission denied");
            if (!fs.dirs.contains(path)) return status(id, 2, "No such file");
            std::string b;
            str(b, new_handle(path));
            return reply(102, id, b);
        }
        case 5: { // READ
            const OpenHandle& h = handles_.at(in.str());
            const std::string& content = h.content != nullptr ? *h.content : all_files().at(h.path);
            std::uint64_t off = in.u64();
            std::uint32_t len = in.u32();
            reads.emplace_back(off, len);
            if (on_read) on_read(h.path, len);
            if (off >= content.size()) return status(id, 1, "EOF");
            std::size_t n = std::min<std::uint64_t>({len, max_data, content.size() - off});
            std::string b;
            str(b, std::string_view(content).substr(off, n));
            return reply(103, id, b);
        }
        case 6: { // WRITE
            const auto h = handles_.find(in.str());
            const std::uint64_t off = in.u64();
            const std::string data = in.str();
            const auto len = static_cast<std::uint32_t>(data.size());
            ++writes_out_;
            write_bytes_out_ += len;
            writes.push_back(Write{.path = h == handles_.end() ? std::string() : h->second.path,
                                   .offset = off,
                                   .length = len,
                                   .out = writes_out_,
                                   .out_bytes = write_bytes_out_});
            if (max_write != 0 && len > max_write) {
                // OpenSSH's sftp-server ends the session on a packet past its limit.
                dropped = true;
                return;
            }
            const auto answer = [&](std::uint32_t code, std::string_view msg) {
                status(id, code, msg);
                write_of_.back() = len;
            };
            if (h == handles_.end()) return answer(4, "Failure");
            if (!h->second.writable || h->second.content == nullptr) return answer(2, "No such file"); // EBADF
            if (read_only) {
                refuse(id);
                write_of_.back() = len;
                return;
            }
            // Into the file's bytes, also once its name is gone (an inode).
            std::string& content = *h->second.content;
            const std::uint64_t at = h->second.append ? content.size() : off;
            const std::uint64_t size = std::max<std::uint64_t>(content.size(), at + len);
            if (quota_bytes != 0 && fs.used() + (size - content.size()) > quota_bytes) return answer(4, "Failure"); // ENOSPC
            if (content.size() < size) content.resize(size, '\0');
            std::memcpy(content.data() + at, data.data(), len);
            return answer(0, "");
        }
        case 4: { // CLOSE
            const auto closed = handles_.find(in.str());
            if (closed != handles_.end()) {
                if (on_close) on_close(closed->second.path);
                if (closed->second.content != nullptr) fs.closed(closed->second.content);
                handles_.erase(closed);
            }
            if (break_on_close) {
                dropped = true;
                return;
            }
            return status(id, 0);
        }
        case 12: { // READDIR
            auto& h = handles_.at(in.str());
            const auto& names = fs.dirs[h.path];
            if (h.pos >= names.size() && !never_eof) return status(id, 1, "EOF");
            std::string b;
            std::size_t n = std::min(readdir_batch, names.size() - h.pos);
            u32(b, static_cast<std::uint32_t>(n));
            for (std::size_t i = 0; i < n; ++i) {
                const std::string& name = names[h.pos + i];
                str(b, name);
                str(b, "-rw-r--r-- 1 u g 0 Jan 1 00:00 " + name);
                std::string full = h.path == "/" ? "/" + name : h.path + "/" + name;
                if (name != "." && name != ".." && links.contains(full)) link_attrs(b);
                else attrs(b, name == "." || name == ".." ? h.path : full, true);
            }
            h.pos += n;
            return reply(104, id, b);
        }
        case 7: // LSTAT
        case 17: { // STAT
            std::string path = in.str();
            if (on_stat) on_stat(path);
            std::string b;
            if (type == 7 && links.contains(path)) {
                link_attrs(b);
                return reply(105, id, b);
            }
            path = resolve(path);
            if (!known(path)) return status(id, 2, "No such file");
            attrs(b, path);
            return reply(105, id, b);
        }
        case 8: { // FSTAT
            const OpenHandle& h = handles_.at(in.str());
            std::string b;
            const std::string now = h.content != nullptr ? fs.path_of(h.content) : h.path;
            if (h.content != nullptr && now.empty()) { // its name is gone: the bytes it still has
                u32(b, 0x1 | 0x4 | 0x8);
                u64(b, h.content->size());
                u32(b, 0100644);
                u32(b, 1700000000);
                u32(b, 1700000000);
            } else {
                attrs(b, now);
            }
            return reply(105, id, b);
        }
        case 9:    // SETSTAT
        case 10: { // FSETSTAT
            std::string path;
            if (type == 9) {
                path = resolve(in.str());
            } else {
                const auto h = handles_.find(in.str());
                if (h == handles_.end()) return status(id, 4, "Failure");
                path = h->second.content != nullptr ? fs.path_of(h->second.content) : h->second.path;
            }
            const sftp::Attrs a = in.attrs();
            if (no_setstat) return status(id, 8, "Operation unsupported");
            if (denied.contains(path)) return status(id, 3, "Permission denied");
            if (!known(path)) return status(id, 2, "No such file");
            if (read_only) return refuse(id);
            if (a.size) {
                if (!fs.files.contains(path)) return status(id, 4, "Failure");
                std::string& content = fs.files[path];
                if (quota_bytes != 0 && *a.size > content.size() && fs.used() + (*a.size - content.size()) > quota_bytes)
                    return status(id, 4, "Failure");
                content.resize(*a.size, '\0');
            }
            if (a.permissions) fs.modes[path] = *a.permissions & 07777;
            if (a.mtime) fs.mtimes[path] = *a.mtime;
            return status(id, 0);
        }
        case 13: { // REMOVE
            const std::string path = in.str();
            if (denied.contains(path)) return status(id, 3, "Permission denied");
            if (read_only) return refuse(id);
            if (fs.dirs.contains(path)) return status(id, 4, "Failure"); // EISDIR
            if (!fs.files.contains(path)) return status(id, 2, "No such file");
            if (locked_dirs.contains(parent_of(path))) return status(id, 3, "Permission denied");
            fs.unlink(path);
            unlisted(path);
            return status(id, 0);
        }
        case 14: { // MKDIR
            const std::string path = in.str();
            const sftp::Attrs a = in.attrs();
            if (denied.contains(path)) return status(id, 3, "Permission denied");
            if (read_only) return refuse(id);
            if (known(path) || links.contains(path)) { // EEXIST
                if (mkdir_existing_as == sftp::Code::permission_denied) return status(id, 3, "Permission denied");
                return status(id, 4, "Failure");
            }
            if (!parent_there(path)) return status(id, 2, "No such file");
            if (locked_dirs.contains(parent_of(path))) return status(id, 3, "Permission denied");
            fs.dirs[path] = {".", ".."};
            if (a.permissions) fs.modes[path] = *a.permissions & 07777;
            listed(path);
            return status(id, 0);
        }
        case 15: { // RMDIR
            const std::string path = in.str();
            if (denied.contains(path)) return status(id, 3, "Permission denied");
            if (read_only) return refuse(id);
            if (!fs.dirs.contains(path)) return status(id, 2, "No such file"); // ENOENT, ENOTDIR
            if (!empty_dir(path)) return status(id, 4, "Failure");             // ENOTEMPTY
            if (locked_dirs.contains(parent_of(path))) return status(id, 3, "Permission denied");
            fs.dirs.erase(path);
            fs.mtimes.erase(path);
            fs.modes.erase(path);
            unlisted(path);
            return status(id, 0);
        }
        case 18: { // RENAME
            const std::string from = in.str();
            const std::string to = in.str();
            return rename(id, from, to, rename_overwrites);
        }
        case 200: { // EXTENDED
            const std::string name = in.str();
            if (bad_extended_reply) {
                std::string b = *bad_extended_reply;
                return reply(201, id, b);
            }
            if (name == "posix-rename@openssh.com" && posix_rename) {
                const std::string from = in.str();
                const std::string to = in.str();
                return rename(id, from, to, true);
            }
            if (name == "statvfs@openssh.com" && statvfs) {
                const std::string path = resolve(in.str());
                if (!known(path)) return status(id, 2, "No such file");
                const std::uint64_t total = quota_bytes != 0 ? quota_bytes : std::uint64_t{1} << 40;
                const std::uint64_t left = total - std::min(total, fs.used());
                // ST_RDONLY for a read-only file system: one that refuses with "Failure" (a folder
                // that refuses with "Permission denied" is on a writable one).
                const bool rdonly = read_only && read_only_as == sftp::Code::failure;
                std::string b;
                for (const std::uint64_t v : {std::uint64_t{4096}, std::uint64_t{4096}, total / 4096, left / 4096, left / 4096,
                                              std::uint64_t{1000000}, std::uint64_t{900000}, std::uint64_t{900000},
                                              std::uint64_t{0x1234}, std::uint64_t{rdonly ? 1u : 0u}, std::uint64_t{255}})
                    u64(b, v);
                return reply(201, id, b);
            }
            if (name == "limits@openssh.com" && max_write != 0) {
                std::string b;
                u64(b, std::uint64_t{256} * 1024);
                u64(b, max_data);
                u64(b, max_write);
                u64(b, 1024);
                return reply(201, id, b);
            }
            if (name == "fsync@openssh.com" && fsync) {
                if (!handles_.contains(in.str())) return status(id, 4, "Failure");
                return status(id, 0);
            }
            return status(id, 8, "Operation unsupported");
        }
        case 16: { // REALPATH
            std::string path = in.str();
            std::string b;
            u32(b, 1);
            str(b, path == "." ? "/home/test" : resolve(path));
            str(b, "");
            u32(b, 0);
            return reply(104, id, b);
        }
        default: return status(id, 8, "Operation unsupported");
        }
    }

    // RENAME as OpenSSH's sftp-server does it (link and unlink: an existing `to` fails it), or,
    // `overwrite`, as rename(2) does (posix-rename): a file replaces a file, a folder an empty one.
    void rename(std::uint32_t id, const std::string& from, const std::string& to, bool overwrite) {
        FakeFiles& fs = st();
        if (denied.contains(from) || denied.contains(to)) return status(id, 3, "Permission denied");
        if (read_only) return refuse(id);
        if (!known(from)) return status(id, 2, "No such file");
        if (!parent_there(to)) return status(id, 2, "No such file");
        if (locked_dirs.contains(parent_of(from)) || locked_dirs.contains(parent_of(to))) return status(id, 3, "Permission denied");
        if (!overwrite && rename_refused.contains(to)) return status(id, 3, "Permission denied");
        if (from == to) return status(id, 0);
        if (known(to)) {
            if (!overwrite) return status(id, 4, "Failure");
            const bool from_dir = fs.dirs.contains(from);
            const bool to_dir = fs.dirs.contains(to);
            if (from_dir != to_dir) return status(id, 4, "Failure");     // EISDIR, ENOTDIR
            if (to_dir && !empty_dir(to)) return status(id, 4, "Failure"); // ENOTEMPTY
            fs.unlink(to);
            fs.dirs.erase(to);
            unlisted(to);
        }
        unlisted(from);
        move_tree(from, to);
        listed(to);
        if (break_after_rename != nullptr && break_after_rename->exchange(false)) {
            dropped = true; // done, and the link gone before the answer
            return;
        }
        return status(id, 0);
    }

    struct OpenHandle {
        std::string path;
        std::size_t pos;
        bool writable;
        bool append;
        std::string* content; // a file of the store's: its bytes (nullptr: a folder, or shared_files)
    };
    std::string in_, out_;
    std::vector<std::string> queue_;
    std::vector<std::chrono::steady_clock::time_point> due_; // when each of queue_ may go (latency)
    std::vector<std::optional<std::uint32_t>> write_of_;     // each of queue_: the length of the WRITE it answers
    std::size_t writes_out_ = 0;                             // WRITEs whose replies haven't gone yet
    std::uint64_t write_bytes_out_ = 0;                      // and their bytes
    std::map<std::string, OpenHandle> handles_;
    int next_handle_ = 1;
    int read_calls_ = 0;
};

// Bytes that differ along the file, so a piece in the wrong place shows.
inline std::string pattern(std::size_t n) {
    std::string s(n, '\0');
    for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 131 + i / 977) & 0xFF);
    return s;
}

} // namespace grab::test
