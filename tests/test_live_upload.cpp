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
#include "engine.hpp"
#include "fetch.hpp"
#include "folder.hpp"
#include "folder_job.hpp"
#include "live_settings.hpp"
#include "rate.hpp"
#include "rclone_encoding.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_pool.hpp"
#include "ssh_walk.hpp"
#include "upload.hpp"
#include "upload_existing.hpp"
#include "upload_folder.hpp"
#include "util.hpp"
#include "verify.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace grab;
namespace fs = std::filesystem;

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

struct Removed {
    int files = 0;
    int folders = 0;
    int kept = 0;
};

// A folder of ours and what is in it, `depth` levels of folders further down at most (deeper ones
// are left and counted). The listing's attributes are LSTAT's (OpenSSH), so a link is removed as
// itself, never followed.
void remove_tree(sftp::Client& s, const std::string& path, int depth, Removed& r) {
    // A folder a case made read-only (up-ro) is made writable again first.
    if (auto opened = sftp::setstat(s, path, sftp::Attrs{.size = {}, .permissions = 0700, .mtime = {}}); !opened) {
        // Not ours to change, or gone: what is left is counted below.
    }
    if (auto inner = sftp::list(s, path)) {
        for (const auto& f : *inner) {
            const std::string child = path + "/" + f.name;
            if (f.attrs.is_dir()) {
                if (depth > 0) remove_tree(s, child, depth - 1, r);
                else ++r.kept;
            } else if (sftp::remove(s, child)) {
                ++r.files;
            } else {
                ++r.kept;
            }
        }
    }
    if (sftp::rmdir(s, path)) ++r.folders;
    else ++r.kept;
}

// The folder's own files (those these cases name: up-*, .grab-probe-*, and upload::run's temps of
// up-* files) and the folders among them with what is in them (3 levels down at most), then the
// folder itself.
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
    Removed r;
    for (const auto& e : *listed) {
        const bool ours = e.name.starts_with("up-") || e.name.starts_with(".grab-probe-") ||
                          (e.name.starts_with(".up-") && upload::is_temp_name(e.name));
        const std::string path = d.raw + "/" + e.name;
        if (!ours) {
            ++r.kept;
            continue;
        }
        if (e.attrs.is_dir()) {
            remove_tree(s, path, 2, r);
        } else if (sftp::remove(s, path)) {
            ++r.files;
        } else {
            ++r.kept;
        }
    }
    auto removed = sftp::rmdir(s, d.raw);
    const bool gone = !sftp::lstat(s, d.raw);
    say(std::format("cleanup: {} file(s) and {} folder(s) removed, {} left; RMDIR of the folder {}; gone: {}", r.files, r.folders, r.kept,
                    code_of(removed), yes(gone)));
}

// ---- upload::run on the server (GRAB_TEST_UPLOAD_RUN) ----------------------------------------

// The uploads' engine as the window and the command will run it: the server's pool and budget,
// local files made up in GRAB_TEST_UPLOAD_LOCAL, their records in a folder there.
struct Uploader {
    std::string server;
    Settings d;
    std::shared_ptr<ssh::Pool> pool;
    fetch::Connections connections;
    fs::path local;
    fs::path records;
    int next_id = 1;
};

const char* outcome_name(upload::Outcome o) {
    switch (o) {
    case upload::Outcome::done: return "done";
    case upload::Outcome::stopped: return "stopped";
    case upload::Outcome::exists: return "exists";
    case upload::Outcome::skipped: return "skipped";
    case upload::Outcome::failed: break;
    }
    return "failed";
}

const char* pass(bool b) { return b ? "PASS" : "FAIL"; }

// A local file of `size` bytes of `data`, made unless it is there with that size already.
fs::path local_data(const Uploader& e, const std::string& name, const Data& data, std::uint64_t size) {
    const fs::path p = e.local / name;
    std::error_code ec;
    if (fs::is_regular_file(p, ec) && fs::file_size(p, ec) == size) return p;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    std::vector<char> buf(Data::block);
    for (std::uint64_t at = 0; at < size; at += buf.size()) {
        const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), size - at));
        data.fill(at, std::span(buf.data(), n));
        out.write(buf.data(), static_cast<std::streamsize>(n));
    }
    out.close();
    const std::string why = "cannot write " + util::path_to_utf8(p);
    REQUIRE_MESSAGE(out.good(), why);
    return p;
}

void set_local_mtime(const fs::path& p, std::chrono::system_clock::time_point t) {
    fs::last_write_time(p, std::chrono::clock_cast<std::chrono::file_clock>(t));
}

// One upload::run, and what came of it.
struct Ran {
    upload::Result result;
    double took = 0;          // s, the whole run
    double sending = 0;       // s, from the first acknowledgement to the last
    std::uint64_t acked = 0;  // bytes the server acknowledged in this run
    std::vector<std::pair<double, std::uint64_t>> acks; // (s from the first acknowledgement, bytes), when kept
};

struct RunOptions {
    upload::Existing existing = upload::Existing::ask;
    rate::RateLimiter* limiter = nullptr;
    std::uint64_t stop_at = 0; // stops once the server acknowledged this much (0: never)
    bool keep_acks = false;
};

Ran run_upload(Uploader& e, const fs::path& local, const std::string& remote, const RunOptions& o) {
    upload::Upload u;
    u.local = local;
    u.server = e.server;
    u.id = e.next_id++;
    u.remote_path = remote;
    u.existing = o.existing;
    u.limiter = o.limiter;
    u.pool = e.pool;
    u.records_dir = e.records;
    Ran ran;
    std::mutex m;
    std::optional<Clock::time_point> first;
    Clock::time_point last;
    std::stop_source stop;
    u.on_sent = [&](std::uint64_t n) {
        std::scoped_lock lock(m);
        const auto now = Clock::now();
        if (!first) first = now;
        last = now;
        ran.acked += n;
        if (o.keep_acks) ran.acks.emplace_back(seconds(now - *first), n);
        if (o.stop_at > 0 && ran.acked >= o.stop_at) stop.request_stop();
    };
    const auto started = Clock::now();
    ran.result = upload::run(u, e.connections, {}, stop.get_token());
    ran.took = seconds(Clock::now() - started);
    if (first) ran.sending = seconds(last - *first);
    return ran;
}

std::string md5_line(Uploader& e, const std::string& remote, const std::string& want) {
    const auto got = server_md5(*e.pool, rclone_enc::raw_path(remote));
    if (!got) return "md5 unavailable (" + got.error() + ")";
    return *got == want ? "md5 equal" : "md5 DIFFERENT";
}

// The temp files of `name` in the folder (upload::run's, by their names).
std::vector<std::string> temps_of(Uploader& e, const std::string& name) {
    auto lease = e.pool->acquire({}, ssh::interactive_retry);
    REQUIRE(lease);
    auto listed = sftp::list((*lease)->sftp(), e.d.raw);
    REQUIRE(listed);
    std::vector<std::string> out;
    for (const auto& entry : *listed)
        if (upload::is_temp_name(entry.name, name)) out.push_back(entry.name);
    return out;
}

void engine_big(Uploader& e) {
    const std::uint64_t size = number_from_env("GRAB_TEST_UPLOAD_MIB", 1024) * mib;
    const Data data(0xB16);
    const fs::path local = local_data(e, "up-big.bin", data, size);
    const std::string remote = in_dir(e.d.dir, "up-big.bin");
    const Ran r = run_upload(e, local, remote, {});
    const double sending = r.sending > 0 ? static_cast<double>(r.acked) / mib / r.sending : 0;
    const double overall = r.took > 0 ? static_cast<double>(size) / mib / r.took : 0;
    say(std::format("big: {} MiB {} in {:.1f} s: {:.2f} MiB/s sending, {:.2f} MiB/s overall (target >= 12.0: 90% of 8 "
                    "connections' 13.3) {}; {}; temps left {}{}",
                    size / mib, outcome_name(r.result.outcome), r.took, sending, overall, pass(sending >= 12.0),
                    md5_line(e, remote, data.md5(size)), temps_of(e, "up-big.bin").size(),
                    r.result.error.empty() ? "" : "; error: " + r.result.error));
}

void engine_small(Uploader& e) {
    for (const auto& [name, size] : {std::pair{std::string("up-small-0.bin"), std::uint64_t{0}},
                                     std::pair{std::string("up-small-1.bin"), std::uint64_t{1024}},
                                     std::pair{std::string("up-small-2.bin"), std::uint64_t{100} * 1024},
                                     std::pair{std::string("up-small-3.bin"), std::uint64_t{1536} * 1024}}) {
        const Data data(size + 7);
        const fs::path local = local_data(e, name, data, size);
        const std::string remote = in_dir(e.d.dir, name);
        const Ran r = run_upload(e, local, remote, {});
        const std::string line = md5_line(e, remote, data.md5(size));
        say(std::format("small: {} ({} bytes) {} in {:.2f} s; {}; no record: {} {}", name, size, outcome_name(r.result.outcome),
                        r.took, line, yes(!upload::saved_progress(e.records, e.server, remote)),
                        pass(r.result.outcome == upload::Outcome::done && line == "md5 equal")));
    }
}

void engine_resume(Uploader& e) {
    const std::uint64_t size = number_from_env("GRAB_TEST_UPLOAD_RESUME_MIB", 256) * mib;
    const Data data(0x2E5);
    const fs::path local = local_data(e, "up-resume.bin", data, size);
    const std::string remote = in_dir(e.d.dir, "up-resume.bin");
    const Ran first = run_upload(e, local, remote, RunOptions{.existing = upload::Existing::ask, .limiter = nullptr,
                                                            .stop_at = size / 2, .keep_acks = false});
    const auto saved = upload::saved_progress(e.records, e.server, remote);
    const auto temps = temps_of(e, "up-resume.bin");
    say(std::format("resume: stopped at {} acknowledged: {}; the record keeps {} of {}; temps on the server {}",
                    first.acked, outcome_name(first.result.outcome), saved ? saved->first : 0, saved ? saved->second : 0, temps.size()));
    if (!saved || first.result.outcome != upload::Outcome::stopped) {
        say("resume: FAIL (no partial upload to go on from)");
        return;
    }
    const Ran second = run_upload(e, local, remote, {});
    const bool none_again = second.acked == size - saved->first;
    say(std::format("resume: the second run {} in {:.1f} s, note \"{}\"; acknowledged {} = the {} the record lacked: {}; {}; "
                    "temps left {}; record gone {} {}",
                    outcome_name(second.result.outcome), second.took, second.result.note, second.acked, size - saved->first,
                    yes(none_again), md5_line(e, remote, data.md5(size)), temps_of(e, "up-resume.bin").size(),
                    yes(!upload::saved_progress(e.records, e.server, remote)),
                    pass(second.result.outcome == upload::Outcome::done && none_again && second.result.note.empty())));
}

void engine_changed(Uploader& e) {
    const std::uint64_t size = std::uint64_t{64} * mib;
    const fs::path local = local_data(e, "up-changed.bin", Data(0xC1), size);
    const std::string remote = in_dir(e.d.dir, "up-changed.bin");
    const Ran first = run_upload(e, local, remote, RunOptions{.existing = upload::Existing::ask, .limiter = nullptr,
                                                            .stop_at = size / 2, .keep_acks = false});
    const auto old_temps = temps_of(e, "up-changed.bin");
    // The local file changed meanwhile: other bytes, a new time.
    const Data changed(0xC2);
    fs::remove(local);
    REQUIRE(local_data(e, "up-changed.bin", changed, size) == local);
    const Ran second = run_upload(e, local, remote, {});
    const std::string line = md5_line(e, remote, changed.md5(size));
    const auto left = temps_of(e, "up-changed.bin");
    say(std::format("changed: first run {} ({} temp), second {} with note \"{}\"; {} (the new bytes); temps left {} {}",
                    outcome_name(first.result.outcome), old_temps.size(), outcome_name(second.result.outcome), second.result.note,
                    line, left.size(),
                    pass(second.result.outcome == upload::Outcome::done && second.result.note.find("started over") != std::string::npos &&
                         line == "md5 equal" && left.empty())));
}

void engine_limit(Uploader& e) {
    const std::uint64_t limit = number_from_env("GRAB_TEST_UPLOAD_LIMIT_MIB", 2) * mib;
    const std::uint64_t size = limit * 26;
    const Data data(0x11);
    const fs::path local = local_data(e, "up-limit.bin", data, size);
    const std::string remote = in_dir(e.d.dir, "up-limit.bin");
    rate::RateLimiter limiter;
    limiter.set_rate(limit);
    const Ran r = run_upload(e, local, remote, RunOptions{.existing = upload::Existing::ask, .limiter = &limiter, .stop_at = 0,
                                                          .keep_acks = true});
    std::uint64_t in_window = 0;
    for (const auto& [at, n] : r.acks)
        if (at >= 3.0 && at < 23.0) in_window += n;
    const double rate = static_cast<double>(in_window) / 20.0 / mib;
    const double want = static_cast<double>(limit) / mib;
    say(std::format("limit: {} MiB/s: {} in {:.1f} s; acknowledged from 3 s to 23 s after the first: {:.3f} MiB/s ({:+.1f} %); {} {}",
                    want, outcome_name(r.result.outcome), r.took, rate, (rate / want - 1) * 100, md5_line(e, remote, data.md5(size)),
                    pass(std::abs(rate / want - 1) <= 0.05)));
}

void engine_rules(Uploader& e) {
    const Data a_data(0xA), b_data(0xB);
    const std::uint64_t a_size = std::uint64_t{300} * 1024, b_size = std::uint64_t{400} * 1024;
    const fs::path a = local_data(e, "up-rules-a.bin", a_data, a_size);
    const fs::path b = local_data(e, "up-rules-b.bin", b_data, b_size);
    const std::string remote = in_dir(e.d.dir, "up-rules.bin");
    const std::string a_md5 = a_data.md5(a_size), b_md5 = b_data.md5(b_size);
    const auto now = std::chrono::system_clock::now();
    set_local_mtime(a, now - std::chrono::hours(1));
    const auto check = [&](const std::string& label, upload::Existing choice, const fs::path& local, upload::Outcome want,
                           const std::string& want_md5, const std::string& want_placed = {}) {
        const Ran r = run_upload(e, local, remote, RunOptions{.existing = choice, .limiter = nullptr, .stop_at = 0, .keep_acks = false});
        const std::string line = md5_line(e, remote, want_md5);
        const bool placed = want_placed.empty() || r.result.placed == want_placed;
        say(std::format("rules: {}: {}{}; the file there: {} {}", label, outcome_name(r.result.outcome),
                        r.result.placed.empty() ? "" : " to " + r.result.placed, line,
                        pass(r.result.outcome == want && line == "md5 equal" && placed)));
    };
    check("a new name", upload::Existing::ask, a, upload::Outcome::done, a_md5, remote);
    check("ask", upload::Existing::ask, b, upload::Outcome::exists, a_md5);
    check("skip", upload::Existing::skip, b, upload::Outcome::skipped, a_md5);
    set_local_mtime(b, now - std::chrono::hours(48));
    check("if newer, older here", upload::Existing::newer, b, upload::Outcome::skipped, a_md5);
    set_local_mtime(b, now);
    check("if newer, newer here", upload::Existing::newer, b, upload::Outcome::done, b_md5, remote);
    check("if different, the same", upload::Existing::different, b, upload::Outcome::skipped, b_md5);
    check("if different, another", upload::Existing::different, a, upload::Outcome::done, a_md5, remote);
    check("replace", upload::Existing::replace, b, upload::Outcome::done, b_md5, remote);
    const Ran both = run_upload(e, a, remote, RunOptions{.existing = upload::Existing::keep_both, .limiter = nullptr, .stop_at = 0,
                                                         .keep_acks = false});
    const std::string numbered = in_dir(e.d.dir, "up-rules (2).bin");
    std::string beside;
    {
        auto lease = e.pool->acquire({}, ssh::interactive_retry);
        REQUIRE(lease);
        auto got = read_small((*lease)->sftp(), rclone_enc::raw_path(numbered));
        beside = got ? md5_of(*got) : "";
    }
    say(std::format("rules: keep both: {} to {}; read back, MD5 equal: {}; the file there still the other: {} {}",
                    outcome_name(both.result.outcome), both.result.placed, yes(beside == a_md5), md5_line(e, remote, b_md5),
                    pass(both.result.outcome == upload::Outcome::done && both.result.placed == numbered && beside == a_md5)));
    // A folder of that name.
    const std::string folder = in_dir(e.d.dir, "up-rules-dir");
    {
        auto lease = e.pool->acquire({}, ssh::interactive_retry);
        REQUIRE(lease);
        REQUIRE(sftp::mkdir((*lease)->sftp(), rclone_enc::raw_path(folder)));
    }
    const Ran over = run_upload(e, a, folder, RunOptions{.existing = upload::Existing::replace, .limiter = nullptr, .stop_at = 0,
                                                        .keep_acks = false});
    say(std::format("rules: a folder there, replace: {} \"{}\" {}", outcome_name(over.result.outcome), over.result.note,
                    pass(over.result.outcome == upload::Outcome::skipped && over.result.note == "a folder of that name is there")));
    const Ran next_to = run_upload(e, a, folder, RunOptions{.existing = upload::Existing::keep_both, .limiter = nullptr, .stop_at = 0,
                                                           .keep_acks = false});
    say(std::format("rules: a folder there, keep both: {} to {} {}", outcome_name(next_to.result.outcome), next_to.result.placed,
                    pass(next_to.result.outcome == upload::Outcome::done && next_to.result.placed == folder + " (2)")));
}

void engine_readonly(Uploader& e) {
    const std::string folder = in_dir(e.d.dir, "up-ro");
    const std::string raw = rclone_enc::raw_path(folder);
    {
        auto lease = e.pool->acquire({}, ssh::interactive_retry);
        REQUIRE(lease);
        auto& s = (*lease)->sftp();
        REQUIRE(sftp::mkdir(s, raw));
        REQUIRE(sftp::setstat(s, raw, sftp::Attrs{.size = {}, .permissions = 0555, .mtime = {}}));
    }
    const std::string want = std::format("{} doesn't let this account write in {}", e.server, folder);
    const auto p = upload::probe(*e.pool, e.server, folder);
    say(std::format(R"(readonly: probe {}: writable {}; "{}"; left "{}" {})", folder, p ? yes(p->writable) : "unknown",
                    p ? p->why : p.error(), p ? p->left : "", pass(p && !p->writable && p->why == want && p->left.empty())));
    const fs::path local = local_data(e, "up-ro-x.bin", Data(0x50), 3 * mib);
    const Ran r = run_upload(e, local, in_dir(folder, "up-x.bin"), {});
    std::size_t inside = 99;
    {
        auto lease = e.pool->acquire({}, ssh::interactive_retry);
        REQUIRE(lease);
        auto& s = (*lease)->sftp();
        if (auto listed = sftp::list(s, raw)) inside = listed->size();
        if (auto back = sftp::setstat(s, raw, sftp::Attrs{.size = {}, .permissions = 0700, .mtime = {}}); !back) {
            say("readonly: the folder couldn't be made writable again: " + code_of(back));
        }
    }
    say(std::format("readonly: upload into it: {} \"{}\"; entries left in it {}; record {} {}", outcome_name(r.result.outcome),
                    r.result.error, inside, yes(upload::saved_progress(e.records, e.server, in_dir(folder, "up-x.bin")).has_value()),
                    pass(r.result.outcome == upload::Outcome::failed && r.result.error == want && inside == 0)));
}

void engine_cancel(Uploader& e) {
    const std::uint64_t size = number_from_env("GRAB_TEST_UPLOAD_RESUME_MIB", 256) * mib;
    const fs::path local = local_data(e, "up-cancel.bin", Data(0xCA), size);
    const std::string remote = in_dir(e.d.dir, "up-cancel.bin");
    const Ran r = run_upload(e, local, remote, RunOptions{.existing = upload::Existing::ask, .limiter = nullptr, .stop_at = size / 4,
                                                        .keep_acks = false});
    const auto before = temps_of(e, "up-cancel.bin");
    const auto gone = upload::discard(*e.pool, e.records, e.server, remote);
    const auto after = temps_of(e, "up-cancel.bin");
    const bool record = upload::saved_progress(e.records, e.server, remote).has_value();
    say(std::format("cancel: {} at {} acknowledged; temps {}; discard {}; temps after {}; record kept {} {}", outcome_name(r.result.outcome),
                    r.acked, before.size(), gone ? std::string("ok") : gone.error(), after.size(), yes(record),
                    pass(r.result.outcome == upload::Outcome::stopped && before.size() == 1 && gone && after.empty() && !record)));
    // A small file has no record: the stopped run hands its temp back, and that is removed.
    const fs::path small_local = local_data(e, "up-cancel-small.bin", Data(0xCB), std::uint64_t{1536} * 1024);
    rate::RateLimiter slow;
    slow.set_rate(std::uint64_t{256} * 1024);
    const std::string small_remote = in_dir(e.d.dir, "up-cancel-small.bin");
    const Ran s = run_upload(e, small_local, small_remote, RunOptions{.existing = upload::Existing::ask, .limiter = &slow, .stop_at = 1,
                                                                    .keep_acks = false});
    const auto removed = s.result.temp.empty() ? std::expected<void, std::string>(std::unexpected(std::string("no temp")))
                                               : upload::remove_temp(*e.pool, s.result.temp);
    const auto left = temps_of(e, "up-cancel-small.bin");
    say(std::format("cancel: a small file {}, its temp handed back: {}; removed {}; temps after {} {}", outcome_name(s.result.outcome),
                    yes(!s.result.temp.empty()), removed ? std::string("ok") : removed.error(), left.size(),
                    pass(s.result.outcome == upload::Outcome::stopped && removed && left.empty())));
}

} // namespace

TEST_CASE("live server: uploads through upload::run, printed only (GRAB_TEST_UPLOAD_RUN)") {
    // GRAB_TEST_SERVER, GRAB_TEST_APPDATA, GRAB_TEST_UPLOAD_DIR as above (made here when missing;
    // 'cleanup' removes it), and:
    //   GRAB_TEST_UPLOAD_RUN    some of big,small,resume,changed,limit,rules,readonly,cancel,cleanup,
    //                           run in that order
    //   GRAB_TEST_UPLOAD_LOCAL  a local folder for the made-up files (kept for the next run) and the
    //                           records (records\)
    //   GRAB_TEST_UPLOAD_BUDGET the server's connections (default: budget::default_budget, 12 for Main)
    //   big: GRAB_TEST_UPLOAD_MIB MiB (1024); resume, cancel: GRAB_TEST_UPLOAD_RESUME_MIB (256);
    //   limit: GRAB_TEST_UPLOAD_LIMIT_MIB MiB/s (2), measured over 20 s
    const auto name = util::getenv_utf8("GRAB_TEST_SERVER");
    const auto appdata = util::getenv_utf8("GRAB_TEST_APPDATA");
    const auto modes = util::getenv_utf8("GRAB_TEST_UPLOAD_RUN");
    const auto local = util::getenv_utf8("GRAB_TEST_UPLOAD_LOCAL");
    if (!name || !appdata || !modes || !local) {
        MESSAGE("skipped: GRAB_TEST_SERVER / GRAB_TEST_APPDATA / GRAB_TEST_UPLOAD_RUN / GRAB_TEST_UPLOAD_LOCAL not set");
        return;
    }
    const auto wanted = [&](std::string_view mode) {
        const auto list = util::split(*modes, ',');
        return std::ranges::find(list, mode) != list.end();
    };
    Uploader e;
    e.server = *name;
    e.d.dir = util::getenv_utf8("GRAB_TEST_UPLOAD_DIR").value_or("");
    const bool in_tmp = allowed_dir(e.d.dir) && e.d.dir.starts_with("/tmp/grab-up-");
    const std::string why = "GRAB_TEST_UPLOAD_DIR must be /tmp/grab-up-* (got \"" + e.d.dir + "\")";
    REQUIRE_MESSAGE(in_tmp, why);
    e.d.raw = rclone_enc::raw_path(e.d.dir);
    e.local = util::path_from_utf8(*local);
    e.records = e.local / "records";
    std::error_code ec;
    REQUIRE_MESSAGE(fs::is_directory(e.local, ec), "GRAB_TEST_UPLOAD_LOCAL must be a folder");

#ifdef _WIN32
    const test::AppData settings(util::path_from_utf8(*appdata));
#endif
    auto ctx = test::read_context(*name);
    REQUIRE_MESSAGE(ctx, ctx.error_or(""));
    auto target = test::live_target(*ctx);
    REQUIRE_MESSAGE(target, target.error_or(""));
    const int budget = static_cast<int>(number_from_env("GRAB_TEST_UPLOAD_BUDGET", static_cast<std::uint64_t>(budget::default_budget(target->host))));
    say(std::format("upload::run on {} ({}); folder {}; {} connections, {} logins at once", *name, *modes, e.d.dir, budget,
                    ssh::Pool::Options{}.logins_at_once));

    std::atomic<int> logins = 0;
    auto o = ssh::Pools::download_options(budget);
    o.idle_timer = false;
    const ssh::Target& t = *target;
    e.pool = std::make_shared<ssh::Pool>([&] {
        ++logins;
        return ssh::open_session(t);
    }, o);
    e.connections.configure(e.server, budget);
    const auto started = Clock::now();
    const bool writes = wanted("big") || wanted("small") || wanted("resume") || wanted("changed") || wanted("limit") ||
                        wanted("rules") || wanted("readonly") || wanted("cancel");
    if (writes) {
        auto lease = e.pool->acquire({}, ssh::interactive_retry);
        REQUIRE(lease);
        if (!ensure_dir((*lease)->sftp(), e.d)) return;
    }
    if (wanted("big")) engine_big(e);
    if (wanted("small")) engine_small(e);
    if (wanted("resume")) engine_resume(e);
    if (wanted("changed")) engine_changed(e);
    if (wanted("limit")) engine_limit(e);
    if (wanted("rules")) engine_rules(e);
    if (wanted("readonly")) engine_readonly(e);
    if (wanted("cancel")) engine_cancel(e);
    if (wanted("cleanup")) {
        auto lease = e.pool->acquire({}, ssh::interactive_retry);
        REQUIRE(lease);
        cleanup((*lease)->sftp(), e.d);
    }
    say(std::format("{} login(s) in all; {:.1f} s", logins.load(), seconds(Clock::now() - started)));
}

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

// ---- folder uploads on the server (GRAB_TEST_UPLOAD_FOLDER) --------------------------------------

namespace {

// One folder upload (upload_folder.hpp) as the window will run it, and what came of it.
struct FolderRun {
    upload::Tree tree;
    std::mutex m;
    upload::FolderJob job;
    std::atomic<std::uint64_t> acked{0};
};

void folder_job(Uploader& e, FolderRun& r, const fs::path& source, const std::string& remote, upload::Existing existing) {
    r.job.connections = &e.connections;
    r.job.files_at_once = [] { return 4; };
    r.job.server = e.server;
    r.job.id = e.next_id++;
    r.job.source = source;
    r.job.remote_path = remote;
    r.job.existing = existing;
    r.job.pool = e.pool;
    r.job.records_dir = e.records;
}

// Walks the local folder (once) and runs the upload until done, or stopped once `stop_at` bytes
// were acknowledged (0: never): its time in seconds.
double run_folder(FolderRun& r, std::uint64_t stop_at = 0) {
    std::stop_source stop;
    r.job.on_sent = [&](std::uint64_t n) {
        if ((r.acked += n) >= stop_at && stop_at > 0) stop.request_stop();
    };
    const auto started = Clock::now();
    auto walked = upload::walk(r.job, r.tree, r.m, {});
    REQUIRE_MESSAGE(walked, walked.error_or(""));
    upload::run(r.job, r.tree, r.m, stop.get_token());
    r.job.on_sent = {};
    return seconds(Clock::now() - started);
}

struct Counts {
    std::size_t done = 0, skipped = 0, failed = 0, waiting = 0;
    std::uint64_t bytes = 0; // of all files
    std::size_t sent = 0;    // done by this upload
};
Counts counts_of(FolderRun& r) {
    Counts c;
    std::lock_guard lock(r.m);
    for (const auto& f : r.tree.files) {
        c.bytes += f.size;
        switch (f.status) {
        case folder::State::done:
            ++c.done;
            if (f.fetched) ++c.sent;
            break;
        case folder::State::skipped: ++c.skipped; break;
        case folder::State::failed: ++c.failed; break;
        default: ++c.waiting; break;
        }
    }
    return c;
}
std::string shown(const Counts& c) {
    return std::format("{} done ({} sent), {} skipped, {} failed, {} waiting", c.done, c.sent, c.skipped, c.failed, c.waiting);
}
// The first failed file's error, for the line.
std::string first_error(FolderRun& r) {
    std::lock_guard lock(r.m);
    const auto o = folder::outcome(r.tree);
    return o.first_error;
}

// What is below `dir` on the server: files, folders, grab's temps, and empty folders.
struct Below {
    bool there = false;
    std::size_t files = 0, folders = 0, temps = 0, empty = 0;
};
Below below_on_server(Uploader& e, const std::string& dir) {
    Below b;
    ssh::WalkOptions o;
    o.connections = 2;
    o.folders = true;
    auto walked = ssh::walk(*e.pool, dir, o, {});
    if (!walked) return b;
    b.there = true;
    std::set<std::string> holding;
    for (const auto& w : walked->entries) {
        const auto slash = w.path.rfind('/');
        if (slash != std::string::npos) holding.insert(w.path.substr(0, slash));
        if (w.dir) {
            ++b.folders;
            continue;
        }
        ++b.files;
        if (upload::is_temp_name(rclone_enc::raw_path(w.path.substr(slash == std::string::npos ? 0 : slash + 1)))) ++b.temps;
    }
    for (const auto& w : walked->entries)
        if (w.dir && !holding.contains(w.path)) ++b.empty;
    return b;
}

// The made-up folder to upload: `small_n` files of 1 to 200 KiB in small\, `big_n` of `big_mib` MiB
// in big\, each of its own bytes; files there with the right size already are kept.
void make_source(const fs::path& source, std::size_t small_n, std::size_t big_n, std::uint64_t big_mib) {
    const Data data(0x5F01);
    std::vector<char> buf(Data::block);
    auto write = [&](const fs::path& p, std::uint64_t offset, std::uint64_t size) {
        std::error_code ec;
        if (fs::is_regular_file(p, ec) && fs::file_size(p, ec) == size) return;
        fs::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        for (std::uint64_t at = 0; at < size; at += buf.size()) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), size - at));
            data.fill(offset + at, std::span(buf.data(), n));
            out.write(buf.data(), static_cast<std::streamsize>(n));
        }
        out.close();
        const std::string why = "cannot write " + util::path_to_utf8(p);
        REQUIRE_MESSAGE(out.good(), why);
    };
    for (std::size_t i = 0; i < small_n; ++i) {
        const std::uint64_t size = (1 + (i * 37) % 200) * 1024;
        write(source / "small" / std::format("s{:04}.bin", i), std::uint64_t{i} * 256 * 1024, size);
    }
    for (std::size_t i = 0; i < big_n; ++i) {
        write(source / "big" / std::format("b{}.bin", i), (std::uint64_t{i} + 1) * (std::uint64_t{1} << 32), big_mib * mib);
    }
}

// grab's own verification of the upload (upload::verify_files): every file sent, by MD5.
std::string folder_verify(const engine::Context& ctx, Uploader& e, FolderRun& r) {
    verify::Request base;
    base.ctx = &ctx;
    base.pool = e.pool;
    const auto checked = upload::verify_files(r.job, r.tree, r.m, base, {});
    const std::size_t same = checked.report.count(verify::Verdict::same);
    return std::format("verify: {} of {} the same: \"{}\" {}", same, checked.files, checked.summary(),
                       pass(checked.files > 0 && same == checked.files));
}

} // namespace

TEST_CASE("live server: folder uploads, printed only (GRAB_TEST_UPLOAD_FOLDER)") {
    // GRAB_TEST_SERVER, GRAB_TEST_APPDATA, GRAB_TEST_UPLOAD_DIR (made here when missing),
    // GRAB_TEST_UPLOAD_LOCAL and GRAB_TEST_UPLOAD_BUDGET as GRAB_TEST_UPLOAD_RUN's, and:
    //   GRAB_TEST_UPLOAD_FOLDER  some of make,up,verify,skip,different,pause,cancel,readonly, in that order
    //   GRAB_TEST_UPLOAD_SOURCE  the local folder uploaded (default GRAB_TEST_UPLOAD_LOCAL\src); it goes
    //                            to GRAB_TEST_UPLOAD_DIR/<its name> (pause: <name>-pause, cancel: <name>-cancel)
    //   make: GRAB_TEST_UPLOAD_SMALL (2000) files of 1-200 KiB in <source>\small and GRAB_TEST_UPLOAD_BIG
    //         (5) of GRAB_TEST_UPLOAD_BIG_MIB (200) MiB in <source>\big, unless they are there
    //   different: GRAB_TEST_UPLOAD_TOUCH (3) of small\'s files (the source's own without one) get
    //              other bytes and a new time first
    //   pause, cancel: stopped once GRAB_TEST_UPLOAD_STOP_PCT (30) % of the bytes are acknowledged
    // verify checks the last upload of this run (up, pause) with the server's md5sum.
    const auto name = util::getenv_utf8("GRAB_TEST_SERVER");
    const auto appdata = util::getenv_utf8("GRAB_TEST_APPDATA");
    const auto modes = util::getenv_utf8("GRAB_TEST_UPLOAD_FOLDER");
    const auto local = util::getenv_utf8("GRAB_TEST_UPLOAD_LOCAL");
    if (!name || !appdata || !modes || !local) {
        MESSAGE("skipped: GRAB_TEST_SERVER / GRAB_TEST_APPDATA / GRAB_TEST_UPLOAD_FOLDER / GRAB_TEST_UPLOAD_LOCAL not set");
        return;
    }
    const auto wanted = [&](std::string_view mode) {
        const auto list = util::split(*modes, ',');
        return std::ranges::find(list, mode) != list.end();
    };
    Uploader e;
    e.server = *name;
    e.d.dir = util::getenv_utf8("GRAB_TEST_UPLOAD_DIR").value_or("");
    const bool in_tmp = allowed_dir(e.d.dir) && e.d.dir.starts_with("/tmp/grab-up-");
    const std::string why = "GRAB_TEST_UPLOAD_DIR must be /tmp/grab-up-* (got \"" + e.d.dir + "\")";
    REQUIRE_MESSAGE(in_tmp, why);
    e.d.raw = rclone_enc::raw_path(e.d.dir);
    e.local = util::path_from_utf8(*local);
    e.records = e.local / "records";
    std::error_code ec;
    REQUIRE_MESSAGE(fs::is_directory(e.local, ec), "GRAB_TEST_UPLOAD_LOCAL must be a folder");
    const auto source_env = util::getenv_utf8("GRAB_TEST_UPLOAD_SOURCE");
    const fs::path source = source_env && !source_env->empty() ? util::path_from_utf8(*source_env) : e.local / "src";
    const std::string source_name = util::path_to_utf8(source.filename());
    const bool plain = allowed_dir(e.d.dir + "/" + source_name);
    REQUIRE_MESSAGE(plain, "the source folder's name must be plain (letters, digits, '.', '-', '_')");

    if (wanted("make")) {
        const auto small_n = static_cast<std::size_t>(number_from_env("GRAB_TEST_UPLOAD_SMALL", 2000));
        const auto big_n = static_cast<std::size_t>(number_from_env("GRAB_TEST_UPLOAD_BIG", 5));
        const std::uint64_t big_mib = number_from_env("GRAB_TEST_UPLOAD_BIG_MIB", 200);
        make_source(source, small_n, big_n, big_mib);
        say(std::format("make: {} with {} small files and {} of {} MiB", util::path_to_utf8(source), small_n, big_n, big_mib));
    }

#ifdef _WIN32
    const test::AppData settings(util::path_from_utf8(*appdata));
#endif
    auto ctx = test::read_context(*name);
    REQUIRE_MESSAGE(ctx, ctx.error_or(""));
    auto target = test::live_target(*ctx);
    REQUIRE_MESSAGE(target, target.error_or(""));
    const int budget = static_cast<int>(number_from_env("GRAB_TEST_UPLOAD_BUDGET", static_cast<std::uint64_t>(budget::default_budget(target->host))));
    say(std::format("folder uploads on {} ({}); folder {}; {} connections, {} logins at once", *name, *modes, e.d.dir, budget,
                    ssh::Pool::Options{}.logins_at_once));
    std::atomic<int> logins = 0;
    auto o = ssh::Pools::download_options(budget);
    o.idle_timer = false;
    const ssh::Target& t = *target;
    e.pool = std::make_shared<ssh::Pool>([&] {
        ++logins;
        return ssh::open_session(t);
    }, o);
    e.connections.configure(e.server, budget);
    const auto started = Clock::now();
    {
        auto lease = e.pool->acquire({}, ssh::interactive_retry);
        REQUIRE(lease);
        if (!ensure_dir((*lease)->sftp(), e.d)) return;
    }
    const std::string remote = e.d.dir + "/" + source_name;
    std::unique_ptr<FolderRun> last; // the upload `verify` checks

    if (wanted("up")) {
        auto r = std::make_unique<FolderRun>();
        folder_job(e, *r, source, remote, upload::Existing::ask);
        const double took = run_folder(*r);
        const Counts c = counts_of(*r);
        const Below b = below_on_server(e, remote);
        const double rate = took > 0 ? static_cast<double>(r->acked.load()) / mib / took : 0;
        say(std::format("up: {} files, {:.1f} MiB in {:.1f} s: {:.2f} MiB/s; {}; temps left {}{} {}", r->tree.files.size(),
                        static_cast<double>(c.bytes) / mib, took, rate, shown(c), b.temps,
                        c.failed > 0 ? "; first error: " + first_error(*r) : "",
                        pass(c.done == r->tree.files.size() && b.temps == 0)));
        last = std::move(r);
    }
    if (wanted("verify") && last) say(folder_verify(*ctx, e, *last));
    if (wanted("skip")) {
        FolderRun r;
        folder_job(e, r, source, remote, upload::Existing::skip);
        const double took = run_folder(r);
        const Counts c = counts_of(r);
        say(std::format("skip: again with Skip: {:.1f} s, {} bytes sent; {} {}", took, r.acked.load(), shown(c),
                        pass(r.acked == 0 && c.sent == 0 && c.skipped == r.tree.files.size())));
    }
    if (wanted("different")) {
        const auto touch = static_cast<std::size_t>(number_from_env("GRAB_TEST_UPLOAD_TOUCH", 3));
        // small\'s files, or the source's own when it has no small\ (a source that is small\ itself).
        const fs::path touch_in = fs::is_directory(source / "small") ? source / "small" : source;
        std::vector<fs::path> small_files;
        for (const auto& entry : fs::directory_iterator(touch_in)) {
            if (entry.is_regular_file()) small_files.push_back(entry.path());
        }
        std::ranges::sort(small_files);
        REQUIRE(small_files.size() >= touch);
        std::uint64_t touched = 0;
        const Data other(0x70C4);
        for (std::size_t i = 0; i < touch; ++i) {
            const fs::path& p = small_files[i * (small_files.size() / touch)];
            const auto size = fs::file_size(p);
            std::string bytes(static_cast<std::size_t>(size), '\0');
            other.fill(std::uint64_t{i} * mib, std::span(bytes.data(), bytes.size()));
            REQUIRE(util::write_file_atomic(p, bytes));
            set_local_mtime(p, std::chrono::system_clock::now());
            touched += size;
        }
        FolderRun r;
        folder_job(e, r, source, remote, upload::Existing::different);
        const double took = run_folder(r);
        const Counts c = counts_of(r);
        say(std::format("different: {} touched ({} bytes), again with Replace if different: {:.1f} s, {} bytes sent; {} {}", touch,
                        touched, took, r.acked.load(), shown(c), pass(c.sent == touch && r.acked == touched && c.failed == 0)));
        last.reset();
    }
    if (wanted("pause")) {
        auto r = std::make_unique<FolderRun>();
        folder_job(e, *r, source, remote + "-pause", upload::Existing::ask);
        std::uint64_t total = 0;
        for (const auto& entry : fs::recursive_directory_iterator(source))
            if (entry.is_regular_file()) total += entry.file_size();
        const std::uint64_t stop_at = total * number_from_env("GRAB_TEST_UPLOAD_STOP_PCT", 30) / 100;
        const double first = run_folder(*r, stop_at);
        const Counts paused = counts_of(*r);
        std::size_t known = 0;
        {
            std::lock_guard lock(r->m);
            known = r->tree.temps.size();
        }
        const Below mid = below_on_server(e, remote + "-pause");
        const double second = run_folder(*r);
        const Counts c = counts_of(*r);
        const Below b = below_on_server(e, remote + "-pause");
        say(std::format("pause: stopped after {:.1f} s ({}; temps there {}, {} known to it); resumed: {:.1f} s, {}; temps left {} {}", first,
                        shown(paused), mid.temps, known, second, shown(c), b.temps,
                        pass(paused.done < r->tree.files.size() && c.done == r->tree.files.size() && b.temps == 0)));
        last = std::move(r);
        if (wanted("verify")) say(folder_verify(*ctx, e, *last));
    }
    if (wanted("cancel")) {
        // The server's own folder in the way: merged into, never removed; one of its own inside.
        const std::string place = remote + "-cancel";
        {
            auto lease = e.pool->acquire({}, ssh::interactive_retry);
            REQUIRE(lease);
            auto& s = (*lease)->sftp();
            REQUIRE(sftp::mkdir(s, rclone_enc::raw_path(place)));
            REQUIRE(sftp::mkdir(s, rclone_enc::raw_path(place + "/keep")));
        }
        FolderRun r;
        folder_job(e, r, source, place, upload::Existing::ask);
        std::uint64_t total = 0;
        for (const auto& entry : fs::recursive_directory_iterator(source))
            if (entry.is_regular_file()) total += entry.file_size();
        run_folder(r, total * number_from_env("GRAB_TEST_UPLOAD_STOP_PCT", 30) / 100);
        const Counts c = counts_of(r);
        const Below mid = below_on_server(e, place);
        std::size_t made = 0;
        {
            std::lock_guard lock(r.m);
            made = r.tree.made.size();
        }
        const auto left = upload::discard(r.job, r.tree, r.m);
        const Below b = below_on_server(e, place);
        Below keep;
        {
            auto lease = e.pool->acquire({}, ssh::interactive_retry);
            REQUIRE(lease);
            const auto there = sftp::lstat((*lease)->sftp(), rclone_enc::raw_path(place + "/keep"));
            keep.there = there && there->is_dir();
        }
        const bool saved = upload::saved_progress(e.records, e.server, place + "/big/b0.bin").has_value();
        say(std::format("cancel: stopped ({}; {} folders made; temps there {}); after the cancel: temps {}, empty folders {} (the "
                        "server's own 'keep' one of them: {}), files {}, leftovers {}, a record kept {} {}",
                        shown(c), made, mid.temps, b.temps, b.empty, yes(keep.there), b.files, left.empty() ? "none" : "SOME",
                        yes(saved), pass(b.temps == 0 && keep.there && b.empty == 1 && left.empty() && !saved)));
    }
    if (wanted("readonly")) {
        const fs::path ro_source = e.local / "ro-src";
        fs::create_directories(ro_source / "ro" / "sub");
        REQUIRE(util::write_file_atomic(ro_source / "ok.txt", "fine"));
        REQUIRE(util::write_file_atomic(ro_source / "ro" / "a.txt", "a"));
        REQUIRE(util::write_file_atomic(ro_source / "ro" / "sub" / "b.txt", "b"));
        local_data(e, "ro-src/ro/big.bin", Data(0x5A), 3 * mib);
        const std::string place = e.d.dir + "/ro-src";
        const std::string ro = place + "/ro";
        {
            auto lease = e.pool->acquire({}, ssh::interactive_retry);
            REQUIRE(lease);
            auto& s = (*lease)->sftp();
            REQUIRE(sftp::mkdir(s, rclone_enc::raw_path(place)));
            REQUIRE(sftp::mkdir(s, rclone_enc::raw_path(ro)));
            REQUIRE(sftp::setstat(s, rclone_enc::raw_path(ro), sftp::Attrs{.size = {}, .permissions = 0555, .mtime = {}}));
        }
        FolderRun r;
        folder_job(e, r, ro_source, place, upload::Existing::ask);
        run_folder(r);
        const std::string want = std::format("{} doesn't let this account write in {}", e.server, ro);
        bool ro_failed = true;
        std::string seen;
        {
            std::lock_guard lock(r.m);
            for (const auto& f : r.tree.files) {
                if (!f.path.starts_with("ro/")) continue;
                ro_failed = ro_failed && f.status == folder::State::failed && f.error == want;
                if (seen.empty()) seen = f.error;
            }
        }
        const Counts c = counts_of(r);
        const Below inside = below_on_server(e, ro);
        {
            auto lease = e.pool->acquire({}, ssh::interactive_retry);
            REQUIRE(lease);
            if (auto back = sftp::setstat((*lease)->sftp(), rclone_enc::raw_path(ro), sftp::Attrs{.size = {}, .permissions = 0700, .mtime = {}});
                !back) {
                say("readonly: the folder couldn't be made writable again: " + code_of(back));
            }
        }
        say(std::format(R"(readonly: {}; the ro\ files: "{}"; entries left in ro {} {})", shown(c), seen, inside.files + inside.folders,
                        pass(ro_failed && c.done == 1 && c.failed == 3 && inside.files + inside.folders == 0)));
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
