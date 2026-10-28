#pragma once

// An SFTP server in memory for the tests (test_sftp.cpp, test_fetch_grab.cpp).

#include "sftp.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace grab::test {

// An SFTP server in memory, answering the client's packets as they are written. Its switches
// reorder the replies, shorten READs, cut the stream into small pieces, refuse paths or corrupt
// the stream; it records the READs it was asked for.
class FakeServer : public sftp::Channel {
public:
    std::map<std::string, std::string> files;               // path -> content
    std::map<std::string, std::vector<std::string>> dirs;   // path -> entry names (". and .." too)
    std::map<std::string, std::uint64_t> listed_size;       // sizes to claim in ATTRS instead
    std::map<std::string, std::uint64_t> readdir_size;      // sizes to claim in folder listings only (a file changed since)
    std::map<std::string, std::string> links;               // path -> where the link points (a full path)
    std::set<std::string> denied;                           // answered with permission denied
    std::uint32_t max_data = sftp::max_read;                // a DATA carries at most this
    bool reverse = false;                                   // answer each batch last first
    std::size_t slice = 0;                                  // hand out at most this many bytes per read
    std::size_t readdir_batch = 2;                          // entries per NAME
    bool corrupt = false;                                   // send a packet claiming 1 GiB
    const std::map<std::string, std::string>* shared_files = nullptr; // instead of files: one copy for many sessions
    const std::map<std::string, std::uint32_t>* shared_mtimes = nullptr; // files' times instead of 1700000000
    std::chrono::milliseconds delay{0};                     // a pause before each read, as a network's
    int broken_after = 0;                                   // the stream breaks on this read (0: never)
    bool dropped = false;                                   // the server closed the connection: every read breaks
    bool break_on_close = false;                            // the connection breaks at a CLOSE, before its reply
    bool never_eof = false;                                 // past the last name, READDIR gets empty NAMEs, not EOF
    std::set<std::string> fifos;                            // named pipes: STAT says so (an OPEN would block a real server)
    std::set<std::string> no_size;                          // files whose ATTRS carry no size

    std::vector<std::pair<std::uint64_t, std::uint32_t>> reads; // (offset, length) of each READ
    std::vector<std::string> opened;                            // the paths of each OPEN
    std::size_t most_waiting = 0;                               // most requests answered in one batch
    std::uint64_t most_read_bytes = 0;                          // most bytes of DATA answered in one batch

    void write(std::string_view bytes) override {
        in_ += bytes;
        while (in_.size() >= 4) {
            std::uint32_t len = be32(in_.data());
            if (in_.size() < 4 + len) break;
            handle(std::string_view(in_).substr(4, len));
            in_.erase(0, 4 + len);
        }
    }

    std::size_t read(std::span<char> buffer) override {
        if (delay.count() > 0) std::this_thread::sleep_for(delay);
        if (broken_after > 0 && ++read_calls_ >= broken_after) throw sftp::Broken("fake server: the connection broke");
        if (dropped) throw sftp::Broken("fake server: the server closed the connection");
        if (out_.empty()) {
            most_waiting = std::max(most_waiting, queue_.size());
            std::uint64_t data = 0;
            for (const std::string& r : queue_) {
                if (r.size() >= 13 && static_cast<std::uint8_t>(r[4]) == 103) data += r.size() - 13; // DATA: its bytes
            }
            most_read_bytes = std::max(most_read_bytes, data);
            if (reverse) std::ranges::reverse(queue_);
            for (const std::string& r : queue_) out_ += r;
            queue_.clear();
        }
        if (corrupt) out_ = std::string("\x40\x00\x00\x00", 4) + "x";
        if (out_.empty()) throw sftp::Broken("fake server: nothing to send");
        std::size_t n = std::min(buffer.size(), out_.size());
        if (slice) n = std::min(n, slice);
        std::memcpy(buffer.data(), out_.data(), n);
        out_.erase(0, n);
        return n;
    }

private:
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
    void reply(std::uint8_t type, std::uint32_t id, const std::string& body) {
        std::string r;
        u32(r, static_cast<std::uint32_t>(1 + 4 + body.size()));
        u8(r, type);
        u32(r, id);
        r += body;
        queue_.push_back(std::move(r));
    }
    void status(std::uint32_t id, std::uint32_t code, std::string_view msg = "") {
        std::string b;
        u32(b, code);
        str(b, msg);
        str(b, "en");
        reply(101, id, b);
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
        if (dirs.contains(path)) {
            u32(b, 0x4 | 0x8);
            u32(b, 040755);
            u32(b, 0);
            u32(b, 1700000000);
            return;
        }
        u32(b, 0x1 | 0x4 | 0x8);
        u64(b, in_listing && readdir_size.contains(path) ? readdir_size[path] : listed_size.contains(path) ? listed_size[path] : all_files().at(path).size());
        u32(b, 0100644);
        u32(b, 0);
        u32(b, shared_mtimes != nullptr && shared_mtimes->contains(path) ? shared_mtimes->at(path) : 1700000000);
    }
    const std::map<std::string, std::string>& all_files() const { return shared_files ? *shared_files : files; }
    [[nodiscard]] bool known(const std::string& path) const {
        return all_files().contains(path) || dirs.contains(path) || fifos.contains(path);
    }
    std::string new_handle(std::string path) {
        std::string h = "h" + std::to_string(next_handle_++);
        handles_[h] = {std::move(path), 0};
        return h;
    }

    void handle(std::string_view packet) {
        auto type = static_cast<std::uint8_t>(packet[0]);
        In in{packet.substr(1)};
        if (type == 1) { // INIT: no id
            std::string r;
            std::string b;
            u32(b, 3);
            str(b, "posix-rename@openssh.com");
            str(b, "1");
            u32(r, static_cast<std::uint32_t>(1 + b.size()));
            u8(r, 2);
            r += b;
            queue_.push_back(std::move(r));
            return;
        }
        std::uint32_t id = in.u32();
        switch (type) {
        case 3: // OPEN
        case 11: { // OPENDIR
            std::string path = resolve(in.str());
            if (type == 3) opened.push_back(path);
            if (denied.contains(path)) return status(id, 3, "Permission denied");
            if (type == 3 ? !all_files().contains(path) : !dirs.contains(path)) return status(id, 2, "No such file");
            std::string b;
            str(b, new_handle(path));
            return reply(102, id, b);
        }
        case 5: { // READ
            const std::string& content = all_files().at(handles_.at(in.str()).path);
            std::uint64_t off = in.u64();
            std::uint32_t len = in.u32();
            reads.emplace_back(off, len);
            if (off >= content.size()) return status(id, 1, "EOF");
            std::size_t n = std::min<std::uint64_t>({len, max_data, content.size() - off});
            std::string b;
            str(b, std::string_view(content).substr(off, n));
            return reply(103, id, b);
        }
        case 4: // CLOSE
            handles_.erase(in.str());
            if (break_on_close) {
                dropped = true;
                return;
            }
            return status(id, 0);
        case 12: { // READDIR
            auto& h = handles_.at(in.str());
            const auto& names = dirs[h.path];
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
            std::string b;
            attrs(b, handles_.at(in.str()).path);
            return reply(105, id, b);
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

    struct OpenHandle {
        std::string path;
        std::size_t pos;
    };
    std::string in_, out_;
    std::vector<std::string> queue_;
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