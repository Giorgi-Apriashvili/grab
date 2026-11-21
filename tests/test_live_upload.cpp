#include <doctest/doctest.h>

// What a server answers grab's SFTP writes, printed for the uploads' chunk 0 (UPLOADS.md step
// 0.4): uploads\bench-main.ps1 runs 'speed' on Main, uploads\box-spike.ps1 the rest on the
// Storage Boxes. The server is loaded from the settings the programs use, read only
// (live_settings.hpp). Writes go only into GRAB_TEST_UPLOAD_DIR, which must be /tmp/grab-up-* (or
// below it) or grab-up-spike-* in the login folder: made here when it isn't there, every file this
// case makes in it removed again, the folder itself by 'cleanup'. The probe also writes one file and
// one folder in each folder it tries, under new random names it creates exclusively (nothing there
// is ever replaced), and removes them at once. Only codes, numbers and yes/no are printed: no
// server text but a refusal's, no name but the folders given. Lines start with "hook: ".
//
//   GRAB_TEST_SERVER        the server's name in grab.conf
//   GRAB_TEST_APPDATA       the %APPDATA% the programs use (a copy of the settings); grab.conf there
//   GRAB_TEST_UPLOAD        some of caps,probe,rename,offsets,speed,cleanup (comma-separated), run in
//                           that order
//   GRAB_TEST_UPLOAD_DIR    the folder the writes go to (grab's form: absolute, or from the login folder)
//   GRAB_TEST_UPLOAD_PROBE  the folders the probe tries, '|' between them ("." the login folder;
//                           default: GRAB_TEST_UPLOAD_DIR)
//   speed: GRAB_TEST_UPLOAD_CONNS (default 1) connections writing one file of GRAB_TEST_UPLOAD_MIB
//          MiB (256) in as many ranges, each GRAB_TEST_UPLOAD_DEPTH (64) WRITEs of
//          GRAB_TEST_UPLOAD_WRITE_KIB KiB (255) and GRAB_TEST_UPLOAD_UNACKED_MIB MiB (8) in flight;
//          GRAB_TEST_UPLOAD_SNDBUF_KIB the sockets' send buffer (0: Windows' own)
//   offsets: GRAB_TEST_UPLOAD_OFFSETS_MIB MiB (16) written by 4 connections at their offsets
//
// At most 4 logins at once.

#include "conn_budget.hpp"
#include "live_settings.hpp"
#include "rclone_encoding.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"
#include "util.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <format>
#include <initializer_list>
#include <latch>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace grab;

namespace {

using Clock = std::chrono::steady_clock;

double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

void say(const std::string& line) {
    std::fputs(("hook: " + line + "\n").c_str(), stdout);
    std::fflush(stdout);
}

constexpr std::uint64_t mib = std::uint64_t{1} << 20;

// The folder the writes may go to: /tmp/grab-up-* or a folder below it, or grab-up-spike-* in the
// login folder, in plain characters (letters, digits, '.', '-', '_': a shell takes the names as
// they are). Nothing else: this case never writes anywhere it didn't make itself.
bool allowed_dir(std::string_view dir) {
    const bool absolute = dir.starts_with('/');
    std::string_view rest = absolute ? dir.substr(1) : dir;
    std::vector<std::string_view> parts;
    for (;;) {
        const auto slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        const bool plain = std::ranges::all_of(part, [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '-' || c == '_';
        });
        if (part.empty() || part == "." || part == ".." || !plain) return false;
        parts.push_back(part);
        if (slash == std::string_view::npos) break;
        rest.remove_prefix(slash + 1);
    }
    if (absolute) return parts.size() >= 2 && parts[0] == "tmp" && parts[1].starts_with("grab-up-") && parts[1].size() > 8;
    return parts.size() == 1 && parts[0].starts_with("grab-up-spike-") && parts[0].size() > 14;
}

// `name` in the folder `dir` (grab's form, "." the login folder), as raw bytes.
std::string in_dir(std::string_view dir, std::string_view name) {
    if (dir.empty() || dir == ".") return std::string(name);
    std::string raw = rclone_enc::raw_path(dir);
    if (!raw.ends_with('/')) raw += '/';
    return raw + std::string(name);
}

// Six random hex digits, for names no earlier run left.
std::string token() {
    std::random_device rd;
    return std::format("{:06x}", rd() & 0xFFFFFF);
}

// A request's outcome, as printed: 0, or the code and the server's own text.
template <class T> std::string code_of(const std::expected<T, sftp::Status>& r) {
    if (r) return "0";
    return std::format("{} \"{}\"", static_cast<std::uint32_t>(r.error().code), rclone_enc::shown_line(r.error().message));
}

// The bytes of the files written: a 4 MiB random block, each 4 MiB of a file XORed with its number,
// so no two blocks of a file (up to 1 GiB) are alike and a server that compresses or deduplicates
// gains nothing.
class Data {
public:
    static constexpr std::size_t block = std::size_t{4} << 20;
    explicit Data(std::uint64_t seed) : block_(block) {
        std::mt19937_64 rng(seed);
        for (std::size_t i = 0; i < block; i += 8) {
            const std::uint64_t v = rng();
            std::memcpy(block_.data() + i, &v, 8);
        }
    }
    void fill(std::uint64_t offset, std::span<char> out) const {
        std::size_t done = 0;
        while (done < out.size()) {
            const std::uint64_t at = offset + done;
            const auto in_block = static_cast<std::size_t>(at % block);
            const std::size_t n = std::min(out.size() - done, block - in_block);
            const auto key = static_cast<char>((at / block) * 0x9D + 1);
            for (std::size_t i = 0; i < n; ++i) out[done + i] = static_cast<char>(block_[in_block + i] ^ key);
            done += n;
        }
    }
    // MD5 of the first `size` bytes, as md5sum prints it.
    [[nodiscard]] std::string md5(std::uint64_t size) const {
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        REQUIRE(ctx != nullptr);
        REQUIRE(EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) == 1);
        std::vector<char> buf(mib);
        for (std::uint64_t at = 0; at < size; at += buf.size()) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), size - at));
            fill(at, std::span(buf.data(), n));
            REQUIRE(EVP_DigestUpdate(ctx, buf.data(), n) == 1);
        }
        std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
        unsigned int len = 0;
        REQUIRE(EVP_DigestFinal_ex(ctx, digest.data(), &len) == 1);
        EVP_MD_CTX_free(ctx);
        std::string out;
        for (unsigned int i = 0; i < len; ++i) out += std::format("{:02x}", digest[i]);
        return out;
    }

private:
    std::vector<char> block_;
};

// MD5 of bytes, as md5sum prints it.
std::string md5_of(std::string_view bytes) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size = 0;
    REQUIRE(EVP_Digest(bytes.data(), bytes.size(), digest.data(), &size, EVP_md5(), nullptr) == 1);
    std::string out;
    for (unsigned int i = 0; i < size; ++i) out += std::format("{:02x}", digest[i]);
    return out;
}

// [0, size) in `parts` ranges, the last taking what is left.
std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges_of(std::uint64_t size, int parts) {
    std::vector<std::pair<std::uint64_t, std::uint64_t>> out;
    const std::uint64_t each = size / static_cast<std::uint64_t>(parts);
    for (int i = 0; i < parts; ++i) {
        const std::uint64_t from = each * static_cast<std::uint64_t>(i);
        out.emplace_back(from, i + 1 == parts ? size - from : each);
    }
    return out;
}

std::uint64_t number_from_env(const char* name, std::uint64_t fallback) {
    const auto v = util::getenv_utf8(name);
    return v && !v->empty() ? std::stoull(*v) : fallback;
}

// The server's md5sum of `raw` (a plain name: letters, digits, '.', '-', '_', '/'), or why not.
std::expected<std::string, std::string> server_md5(ssh::Pool& pool, const std::string& raw) {
    try {
        auto lease = pool.acquire({}, ssh::interactive_retry);
        if (!lease) return std::unexpected(std::string("no connection"));
        std::string out;
        ssh::ExecOptions o;
        o.out = [&](std::string_view piece) {
            out.append(piece.substr(0, 4096 - std::min<std::size_t>(4096, out.size())));
            return true;
        };
        o.dead_after = std::chrono::hours(1);
        o.deadline = std::chrono::minutes(10);
        auto r = (*lease)->exec("md5sum " + raw, o);
        if (!r) return std::unexpected("not run: " + r.error().message);
        if (r->status != 0 || out.size() < 32) return std::unexpected(std::format("exit {}", r->status));
        return out.substr(0, 32);
    } catch (const ssh::Error& e) {
        return std::unexpected(std::string("connection: ") + e.what());
    }
}

// A small file's bytes read over SFTP (at most `most`), or the failure's code.
std::expected<std::string, sftp::Status> read_small(sftp::Client& s, const std::string& raw, std::uint64_t most = mib) {
    auto h = sftp::open_file(s, raw);
    if (!h) return std::unexpected(h.error());
    auto attrs = sftp::fstat(s, *h);
    std::string got(static_cast<std::size_t>(std::min<std::uint64_t>(attrs && attrs->size ? *attrs->size : 0, most)), '\0');
    auto r = sftp::read_range(s, *h, 0, got.size(), [&](std::uint64_t at, std::string_view bytes) {
        std::memcpy(got.data() + at, bytes.data(), bytes.size());
    });
    if (auto closed = sftp::close_file(s, *h); !closed) {
        // A handle that fails to close doesn't spoil what was read.
    }
    if (!r) return std::unexpected(r.error());
    return got;
}

// A new file of `data` (created exclusively), closed.
std::expected<void, sftp::Status> create_file(sftp::Client& s, const std::string& raw, std::string_view data) {
    auto h = sftp::open_file(s, raw, sftp::OpenFlags::write | sftp::OpenFlags::creat | sftp::OpenFlags::excl);
    if (!h) return std::unexpected(h.error());
    auto written = sftp::write_range(s, *h, 0, data.size(),
                                     [&](std::uint64_t at, std::uint32_t n) { return data.substr(at, n); });
    auto closed = sftp::close_file(s, *h);
    if (!written) return written;
    return closed;
}

const char* yes(bool b) { return b ? "yes" : "NO"; }

struct Settings {
    std::string dir;   // GRAB_TEST_UPLOAD_DIR, grab's form ("" when not given)
    std::string raw;   // the same as raw bytes
    bool made = false; // this run made it
};

// GRAB_TEST_UPLOAD_DIR there as a folder: made when missing. false: it can't be used.
bool ensure_dir(sftp::Client& s, Settings& d) {
    auto there = sftp::lstat(s, d.raw);
    if (there && there->is_dir()) {
        say(std::format("folder {}: there already", d.dir));
        return true;
    }
    if (there) {
        say(std::format("folder {}: there, but not a folder: nothing written", d.dir));
        return false;
    }
    auto made = sftp::mkdir(s, d.raw, sftp::Attrs{.size = {}, .permissions = 0700, .mtime = {}});
    say(std::format("folder {}: MKDIR {}", d.dir, code_of(made)));
    d.made = made.has_value();
    return d.made;
}

void caps(sftp::Client& s, const Settings& d) {
    std::string offered;
    for (const auto& e : s.extensions()) offered += (offered.empty() ? "" : ", ") + e.name + " " + rclone_enc::shown_line(e.data);
    say(std::format("caps: SFTP version {}; extensions: {}", s.version(), offered.empty() ? "none" : offered));
    if (s.has_extension("limits@openssh.com")) {
        auto l = sftp::limits(s);
        if (l) say(std::format("caps: limits: packet {}, read {}, write {}, handles {}", l->packet, l->read, l->write, l->handles));
        else say("caps: limits: " + code_of(l));
    } else {
        say("caps: limits@openssh.com not offered");
    }
    auto home = sftp::realpath(s, ".");
    say(std::format("caps: REALPATH . {}", code_of(home)));
    std::vector<std::pair<std::string, std::string>> where{{".", "."}};
    if (!d.dir.empty()) where.emplace_back(d.dir, d.raw);
    for (const auto& [label, raw] : where) {
        if (!s.has_extension("statvfs@openssh.com")) {
            say("caps: statvfs@openssh.com not offered");
            break;
        }
        auto v = sftp::statvfs(s, raw);
        if (!v) {
            say(std::format("caps: statvfs({}): {}", label, code_of(v)));
            continue;
        }
        say(std::format("caps: statvfs({}): bsize {}, frsize {}, blocks {}, bavail {} = {} free; flag {} (read-only {}); "
                        "namemax {}",
                        label, v->bsize, v->frsize, v->blocks, v->bavail, util::format_size(v->free_bytes()), v->flag,
                        v->read_only() ? "yes" : "no", v->namemax));
    }
}

// The window's write check (UPLOADS.md 5.10) in `dir`: a file of its own made exclusively, written,
// its time set, closed, checked and removed; a folder of its own made and removed. Each answer's
// code (and the server's text for a refusal). false: something it made is still there.
bool probe(sftp::Client& s, std::string_view dir) {
    const std::string name = ".grab-probe-" + token();
    const std::string file = in_dir(dir, name);
    bool left = false;
    auto h = sftp::open_file(s, file, sftp::OpenFlags::write | sftp::OpenFlags::creat | sftp::OpenFlags::excl,
                             sftp::Attrs{.size = {}, .permissions = 0600, .mtime = {}});
    std::string line = std::format("probe {}: OPEN (create, exclusive) {}", dir, code_of(h));
    if (h) {
        std::optional<std::expected<void, sftp::Status>> wrote;
        s.write(*h, 0, "probe", [&](std::expected<void, sftp::Status> r) { wrote = std::move(r); });
        while (!wrote) s.pump();
        auto timed = sftp::fsetstat(s, *h, sftp::Attrs{.size = {}, .permissions = {}, .mtime = 1600000000});
        auto closed = sftp::close_file(s, *h);
        auto attrs = sftp::stat(s, file);
        const bool kept = attrs && attrs->size == 5u && attrs->mtime == 1600000000u;
        auto removed = sftp::remove(s, file);
        auto gone = sftp::lstat(s, file);
        const bool is_gone = !gone && gone.error().code == sftp::Code::no_such_file;
        left = !is_gone;
        line += std::format("; WRITE {}; FSETSTAT (time) {}; CLOSE {}; size and time kept: {}; REMOVE {}; gone: {}", code_of(*wrote),
                            code_of(timed), code_of(closed), yes(kept), code_of(removed), yes(is_gone));
    }
    say(line);
    const std::string folder = in_dir(dir, name + ".d");
    auto made = sftp::mkdir(s, folder);
    line = std::format("probe {}: MKDIR {}", dir, code_of(made));
    if (made) {
        auto removed = sftp::rmdir(s, folder);
        auto gone = sftp::lstat(s, folder);
        const bool is_gone = !gone && gone.error().code == sftp::Code::no_such_file;
        left = left || !is_gone;
        line += std::format("; RMDIR {}; gone: {}", code_of(removed), yes(is_gone));
    }
    say(line);
    if (s.has_extension("statvfs@openssh.com")) {
        auto v = sftp::statvfs(s, dir.empty() ? std::string(".") : rclone_enc::raw_path(dir));
        if (v) say(std::format("probe {}: statvfs {} free, read-only flag {}", dir, util::format_size(v->free_bytes()), v->read_only() ? "yes" : "no"));
        else say(std::format("probe {}: statvfs {}", dir, code_of(v)));
    }
    if (left) say(std::format("probe {}: LEFT BEHIND: {} (remove it by hand)", dir, name));
    return !left;
}

// RENAME and the other answers uploads build on (UPLOADS.md 5.3): never replacing, posix-rename
// replacing, the folder requests' failures.
void renames(sftp::Client& s, const Settings& d) {
    const std::string base = "up-rename-" + token();
    const std::string a = in_dir(d.dir, base + "-a"), b = in_dir(d.dir, base + "-b"), c = in_dir(d.dir, base + "-c");
    const std::string sub = in_dir(d.dir, base + "-d");
    const auto remove_all = [&](std::initializer_list<std::string> files) {
        for (const std::string& f : files) {
            if (auto removed = sftp::remove(s, f); !removed) {
                // Not there (moved, or never made): what is left is checked after.
            }
        }
    };
    if (!create_file(s, a, "AAA") || !create_file(s, b, "BB")) {
        say("rename: the two files couldn't be made");
        remove_all({a, b});
        return;
    }
    auto onto = sftp::rename(s, a, b);
    auto bytes_b = read_small(s, b);
    auto bytes_a = read_small(s, a);
    say(std::format("rename: RENAME onto an existing file {}; that file unchanged: {}; the source still there: {}", code_of(onto),
                    yes(bytes_b && *bytes_b == "BB"), yes(bytes_a && *bytes_a == "AAA")));
    if (s.has_extension("posix-rename@openssh.com")) {
        auto replaced = sftp::posix_rename(s, a, b);
        bytes_b = read_small(s, b);
        auto gone = sftp::lstat(s, a);
        say(std::format("rename: posix-rename onto an existing file {}; it holds the source's bytes: {}; the source gone: {}",
                        code_of(replaced), yes(bytes_b && *bytes_b == "AAA"), yes(!gone)));
    } else {
        say("rename: posix-rename@openssh.com not offered");
    }
    auto fresh = sftp::rename(s, b, c);
    auto bytes_c = read_small(s, c);
    say(std::format("rename: RENAME to a new name {}; moved: {}", code_of(fresh), yes(bytes_c && !sftp::lstat(s, b))));
    auto nowhere = sftp::rename(s, c, in_dir(d.dir, base + "-none/x"));
    say(std::format("rename: RENAME into a missing folder {}", code_of(nowhere)));
    auto made = sftp::mkdir(s, sub);
    auto twice = sftp::mkdir(s, sub);
    auto onto_file = sftp::mkdir(s, c);
    say(std::format("rename: MKDIR {}; again {}; onto a file {}", code_of(made), code_of(twice), code_of(onto_file)));
    auto inside = create_file(s, in_dir(d.dir, base + "-d/x"), "x");
    auto full = sftp::rmdir(s, sub);
    auto dir_removed = sftp::remove(s, sub);
    say(std::format("rename: RMDIR of a folder with a file {}; REMOVE of a folder {}", code_of(full), code_of(dir_removed)));
    auto missing = sftp::remove(s, in_dir(d.dir, base + "-none"));
    say(std::format("rename: REMOVE of a missing name {}", code_of(missing)));
    // Everything made here goes again.
    say(std::format("rename: a file in that folder {}", code_of(inside)));
    remove_all({in_dir(d.dir, base + "-d/x"), a, b, c});
    if (auto removed = sftp::rmdir(s, sub); !removed) {
        // Never made, or still holding something: the check below says so.
    }
    bool clean = true;
    for (const std::string& f : {a, b, c, sub}) clean = clean && !sftp::lstat(s, f);
    say(std::format("rename: everything it made removed: {}", yes(clean)));
}

// One connection's part of a file written at its offsets: OPEN (no creat, no trunc), write_range,
// CLOSE. Its bytes, its time from the start to the last acknowledgement, or what failed.
struct Part {
    std::uint64_t bytes = 0;
    double took = 0;
    std::string failed;
    int login_retries = 0;
};

// A lease, its login tried again (2 s apart, up to 3 times in all) when it failed for a reason
// that may pass: a busy server drops a connect now and then among many logins in a row. Before
// the start, so the time measured doesn't include it.
std::optional<ssh::Pool::Lease> acquire_retrying(ssh::Pool& pool, int& retries) {
    for (int attempt = 1;; ++attempt) {
        try {
            return pool.acquire({}, ssh::interactive_retry);
        } catch (const ssh::Error& e) {
            if (e.kind == budget::Failure::fatal || attempt == 3) throw;
            ++retries;
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
}

// `conns` connections write the file `raw` (already made, empty) of `size` bytes in as many ranges,
// each with its own lease, all starting together once every one has its login and handle. The
// parts and the time from the start to the last acknowledgement.
std::pair<std::vector<Part>, double> write_parallel(ssh::Pool& pool, const std::string& raw, std::uint64_t size, int conns,
                                                    const Data& data, int depth, std::uint64_t unacked, std::uint32_t chunk) {
    const auto ranges = ranges_of(size, conns);
    std::vector<Part> parts(static_cast<std::size_t>(conns));
    std::latch ready(conns);
    std::latch go(1);
    std::atomic<std::int64_t> last_ns{0};
    Clock::time_point started;
    std::vector<std::jthread> threads;
    for (int i = 0; i < conns; ++i) {
        threads.emplace_back([&, i] {
            Part& p = parts[static_cast<std::size_t>(i)];
            bool counted = false;
            const auto arrive = [&] {
                if (!counted) ready.count_down();
                counted = true;
            };
            try {
                auto lease = acquire_retrying(pool, p.login_retries);
                if (!lease) {
                    p.failed = "no connection";
                    arrive();
                    return;
                }
                try {
                    auto& s = (*lease)->sftp();
                    auto h = sftp::open_file(s, raw, sftp::OpenFlags::write);
                    arrive();
                    go.wait();
                    if (!h) {
                        p.failed = "OPEN " + code_of(h);
                        return;
                    }
                    const auto [from, length] = ranges[static_cast<std::size_t>(i)];
                    std::vector<char> buf(chunk);
                    auto r = sftp::write_range(
                        s, *h, from, length,
                        [&](std::uint64_t at, std::uint32_t n) {
                            data.fill(at, std::span(buf.data(), n));
                            return std::string_view(buf.data(), n);
                        },
                        [&](std::uint64_t, std::uint32_t n) { p.bytes += n; }, depth, unacked, chunk);
                    const auto now = Clock::now();
                    p.took = seconds(now - started);
                    std::int64_t ns = (now - started).count();
                    for (std::int64_t seen = last_ns.load(); ns > seen && !last_ns.compare_exchange_weak(seen, ns);) {
                    }
                    if (!r) p.failed = "WRITE " + code_of(r);
                    if (auto closed = sftp::close_file(s, *h); !closed && p.failed.empty()) p.failed = "CLOSE " + code_of(closed);
                } catch (const sftp::Broken& e) {
                    lease->broken();
                    p.failed = std::string("connection lost: ") + e.what();
                    arrive();
                }
            } catch (const ssh::Error& e) {
                p.failed = std::string("no login: ") + e.what();
                arrive();
            }
        });
    }
    ready.wait();
    started = Clock::now();
    go.count_down();
    threads.clear();
    return {std::move(parts), std::chrono::duration<double>(Clock::duration(last_ns.load())).count()};
}

void offsets(ssh::Pool& pool, const Settings& d) {
    const std::uint64_t size = number_from_env("GRAB_TEST_UPLOAD_OFFSETS_MIB", 16) * mib;
    const std::string raw = in_dir(d.dir, "up-offsets-" + token() + ".bin");
    const Data data(0x0FF5E75);
    {
        auto lease = pool.acquire({}, ssh::interactive_retry);
        REQUIRE(lease);
        auto made = create_file((*lease)->sftp(), raw, "");
        if (!made) {
            say("offsets: the file couldn't be made: " + code_of(made));
            return;
        }
    }
    const auto [parts, took] = write_parallel(pool, raw, size, 4, data, 64, sftp::max_unacked, sftp::max_write);
    std::string failed;
    for (const Part& p : parts)
        if (!p.failed.empty()) failed += (failed.empty() ? "" : "; ") + p.failed;
    const std::string want = data.md5(size);
    std::string line = std::format("offsets: 4 connections x {} at their offsets in {:.2f} s{}", util::format_size(size / 4), took,
                                   failed.empty() ? "" : "; FAILED: " + failed);
    {
        auto lease = pool.acquire({}, ssh::interactive_retry);
        REQUIRE(lease);
        auto& s = (*lease)->sftp();
        auto attrs = sftp::stat(s, raw);
        line += std::format("; size right: {}", yes(attrs && attrs->size == size));
        auto got = read_small(s, raw, size);
        line += std::format("; read back over SFTP, MD5 equal: {}", got ? yes(md5_of(*got) == want) : "READ " + code_of(got));
    }
    const auto server = server_md5(pool, raw);
    line += server ? std::format("; server md5sum equal: {}", yes(*server == want)) : "; server md5sum: " + server.error();
    say(line);
    auto lease = pool.acquire({}, ssh::interactive_retry);
    REQUIRE(lease);
    auto removed = sftp::remove((*lease)->sftp(), raw);
    say(std::format("offsets: REMOVE {}", code_of(removed)));
}

void speed(ssh::Pool& pool, const Settings& d, int conns) {
    const std::uint64_t size = number_from_env("GRAB_TEST_UPLOAD_MIB", 256) * mib;
    const int depth = static_cast<int>(number_from_env("GRAB_TEST_UPLOAD_DEPTH", 64));
    const auto chunk = static_cast<std::uint32_t>(number_from_env("GRAB_TEST_UPLOAD_WRITE_KIB", 255) * 1024);
    const std::uint64_t unacked = number_from_env("GRAB_TEST_UPLOAD_UNACKED_MIB", 8) * mib;
    const std::uint64_t sndbuf = number_from_env("GRAB_TEST_UPLOAD_SNDBUF_KIB", 0);
    const std::string raw = in_dir(d.dir, "up-speed-" + token() + ".bin");
    const Data data(0x5BEED);
    {
        auto lease = pool.acquire({}, ssh::interactive_retry);
        REQUIRE(lease);
        auto made = create_file((*lease)->sftp(), raw, "");
        if (!made) {
            say("speed: the file couldn't be made: " + code_of(made));
            return;
        }
    }
    const auto [parts, took] = write_parallel(pool, raw, size, conns, data, depth, unacked, std::min(chunk, sftp::max_write));
    std::uint64_t bytes = 0;
    std::string each;
    std::string failed;
    int retries = 0;
    for (const Part& p : parts) {
        bytes += p.bytes;
        retries += p.login_retries;
        each += std::format("{}{:.2f}", each.empty() ? "" : ", ", p.took > 0 ? static_cast<double>(p.bytes) / mib / p.took : 0.0);
        if (!p.failed.empty()) failed += (failed.empty() ? "" : "; ") + p.failed;
    }
    const auto server = server_md5(pool, raw);
    const std::string md5 = !server ? "unavailable (" + server.error() + ")" : *server == data.md5(size) ? "equal" : "DIFFERENT";
    say(std::format("speed: conns {}, depth {}, write {} KiB, unacked {} MiB, sndbuf {} KiB: {} MiB in {:.2f} s = {:.2f} MiB/s "
                    "(per connection: {}{}); md5 {}{}",
                    conns, depth, std::min(chunk, sftp::max_write) / 1024, unacked / mib, sndbuf, bytes / mib, took,
                    took > 0 ? static_cast<double>(bytes) / mib / took : 0.0, each,
                    retries > 0 ? std::format("; logins tried again: {}", retries) : "", md5, failed.empty() ? "" : "; FAILED: " + failed));
    auto lease = pool.acquire({}, ssh::interactive_retry);
    REQUIRE(lease);
    auto removed = sftp::remove((*lease)->sftp(), raw);
    say(std::format("speed: REMOVE {}", code_of(removed)));
}

// The folder's own files (those this case names: up-*, .grab-probe-*) and the folders among them,
// one level down, then the folder itself.
void cleanup(sftp::Client& s, const Settings& d) {
    // A real folder only: OPENDIR follows a link, which would list (and remove in) its target.
    auto there = sftp::lstat(s, d.raw);
    if (!there || !there->is_dir()) {
        say(std::format("cleanup: folder {}: {}: nothing removed", d.dir, !there ? code_of(there) : std::string("there, but not a folder")));
        return;
    }
    auto listed = sftp::list(s, d.raw);
    if (!listed) {
        say(std::format("cleanup: folder {}: {}", d.dir, code_of(listed)));
        return;
    }
    int files = 0;
    int folders = 0;
    int kept = 0;
    for (const auto& e : *listed) {
        const bool ours = e.name.starts_with("up-") || e.name.starts_with(".grab-probe-");
        const std::string path = d.raw + "/" + e.name;
        if (!ours) {
            ++kept;
            continue;
        }
        if (e.attrs.is_dir()) {
            if (auto inner = sftp::list(s, path)) {
                for (const auto& f : *inner) {
                    if (!f.attrs.is_dir() && sftp::remove(s, path + "/" + f.name)) ++files;
                }
            }
            if (sftp::rmdir(s, path)) ++folders;
            else ++kept;
        } else if (sftp::remove(s, path)) {
            ++files;
        } else {
            ++kept;
        }
    }
    auto removed = sftp::rmdir(s, d.raw);
    const bool gone = !sftp::lstat(s, d.raw);
    say(std::format("cleanup: {} file(s) and {} folder(s) removed, {} left; RMDIR of the folder {}; gone: {}", files, folders, kept,
                    code_of(removed), yes(gone)));
}

} // namespace

TEST_CASE("live server: SFTP writes, printed only (GRAB_TEST_UPLOAD)") {
    const auto name = util::getenv_utf8("GRAB_TEST_SERVER");
    const auto appdata = util::getenv_utf8("GRAB_TEST_APPDATA");
    const auto modes = util::getenv_utf8("GRAB_TEST_UPLOAD");
    if (!name || !appdata || !modes) {
        MESSAGE("skipped: GRAB_TEST_SERVER / GRAB_TEST_APPDATA / GRAB_TEST_UPLOAD not set");
        return;
    }
    const auto wanted = [&](std::string_view mode) {
        const auto list = util::split(*modes, ',');
        return std::ranges::find(list, mode) != list.end();
    };
    Settings d;
    d.dir = util::getenv_utf8("GRAB_TEST_UPLOAD_DIR").value_or("");
    const bool writes = wanted("rename") || wanted("offsets") || wanted("speed") || wanted("cleanup");
    if (!d.dir.empty() || writes) {
        const std::string why = "GRAB_TEST_UPLOAD_DIR must be /tmp/grab-up-* or grab-up-spike-* (got \"" + d.dir + "\")";
        REQUIRE_MESSAGE(allowed_dir(d.dir), why);
        d.raw = rclone_enc::raw_path(d.dir);
    }
    std::vector<std::string> probed;
    for (const auto& p : util::split(util::getenv_utf8("GRAB_TEST_UPLOAD_PROBE").value_or(""), '|'))
        if (!p.empty()) probed.emplace_back(p);
    if (probed.empty() && !d.dir.empty()) probed.push_back(d.dir);
    const bool probe_has_folders = !wanted("probe") || !probed.empty();
    REQUIRE_MESSAGE(probe_has_folders, "the probe needs GRAB_TEST_UPLOAD_PROBE or GRAB_TEST_UPLOAD_DIR");

#ifdef _WIN32
    const test::AppData settings(util::path_from_utf8(*appdata));
#endif
    auto ctx = test::read_context(*name);
    REQUIRE_MESSAGE(ctx, ctx.error_or(""));
    auto target = test::live_target(*ctx);
    REQUIRE_MESSAGE(target, target.error_or(""));
    const int conns = static_cast<int>(std::clamp<std::uint64_t>(number_from_env("GRAB_TEST_UPLOAD_CONNS", 1), 1, 16));
    target->send_buffer = static_cast<int>(number_from_env("GRAB_TEST_UPLOAD_SNDBUF_KIB", 0) * 1024);
    say(std::format("uploads on {} ({}); folder {}", *name, *modes, d.dir.empty() ? "none" : d.dir));

    std::atomic<int> logins = 0;
    ssh::Pool::Options o;
    o.limit = std::max(4, conns);
    o.logins_at_once = 4;
    o.idle_timer = false;
    const ssh::Target& t = *target;
    auto pool = std::make_shared<ssh::Pool>([&] {
        ++logins;
        return ssh::open_session(t);
    }, o);
    const auto started = Clock::now();
    {
        auto lease = pool->acquire({}, ssh::interactive_retry);
        REQUIRE(lease);
        auto& s = (*lease)->sftp();
        // The folder first, when anything but the cleanup writes in it.
        const bool probes_dir = wanted("probe") && std::ranges::find(probed, d.dir) != probed.end();
        if (!d.dir.empty() && (probes_dir || wanted("rename") || wanted("offsets") || wanted("speed")) && !ensure_dir(s, d))
            return;
        if (wanted("caps")) caps(s, d);
        if (wanted("probe")) {
            for (const std::string& p : probed) CHECK(probe(s, p));
        }
        if (wanted("rename")) renames(s, d);
    }
    if (wanted("offsets")) offsets(*pool, d);
    if (wanted("speed")) speed(*pool, d, conns);
    if (wanted("cleanup")) {
        auto lease = pool->acquire({}, ssh::interactive_retry);
        REQUIRE(lease);
        cleanup((*lease)->sftp(), d);
    }
    say(std::format("{} login(s) in all; {:.1f} s", logins.load(), seconds(Clock::now() - started)));
}

TEST_CASE("hook helpers: the upload folder's rule, names in it, the test files' bytes and ranges") {
    CHECK(allowed_dir("/tmp/grab-up-bench-1"));
    CHECK(allowed_dir("/tmp/grab-up-x/sub"));
    CHECK(allowed_dir("grab-up-spike-12ab"));
    CHECK_FALSE(allowed_dir("/tmp/grab-up-x/../y"));
    CHECK_FALSE(allowed_dir("/tmp/other"));
    CHECK_FALSE(allowed_dir("/tmp"));
    CHECK_FALSE(allowed_dir("/home/dragon/grab-up-x"));
    CHECK_FALSE(allowed_dir("grab-up-spike-"));
    CHECK_FALSE(allowed_dir("grab-up-spike-1/sub"));
    CHECK_FALSE(allowed_dir("docs"));
    CHECK_FALSE(allowed_dir(""));
    CHECK_FALSE(allowed_dir("/tmp//grab-up-x"));
    CHECK_FALSE(allowed_dir("/tmp/grab-up-x/"));

    CHECK(in_dir(".", "f") == "f");
    CHECK(in_dir("", "f") == "f");
    CHECK(in_dir("/tmp/grab-up-x", "f") == "/tmp/grab-up-x/f");
    CHECK(in_dir("software", ".p") == "software/.p");
    CHECK(token().size() == 6);

    const Data data(7);
    std::vector<char> a(100);
    std::vector<char> b(100);
    data.fill(Data::block - 50, a);
    data.fill(Data::block - 50, std::span(b.data(), 50));
    data.fill(Data::block, std::span(b.data() + 50, 50));
    CHECK(a == b); // the same bytes however asked, across a block's end
    std::vector<char> next(100);
    data.fill(2 * Data::block - 50, next);
    CHECK(next != a); // the next block differs
    std::string whole(Data::block + 100, '\0');
    data.fill(0, std::span(whole.data(), whole.size()));
    CHECK(data.md5(whole.size()) == md5_of(whole));

    CHECK(ranges_of(10, 3) == std::vector<std::pair<std::uint64_t, std::uint64_t>>{{0, 3}, {3, 3}, {6, 4}});
    CHECK(ranges_of(8, 1) == std::vector<std::pair<std::uint64_t, std::uint64_t>>{{0, 8}});
}
