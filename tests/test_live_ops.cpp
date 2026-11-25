#include <doctest/doctest.h>

// What a server answers grab's own copies, deletes, owners' names and links, printed for the server
// operations' chunk 0 (SERVER-OPS.md): grab-plans\server-ops\ops-spike.ps1 runs it on the Storage
// Boxes (the user), the live checker on Main. The server is loaded from the settings the programs
// use, read only (live_settings.hpp). Writes go only into GRAB_TEST_OPS_DIR, which must be
// /tmp/grab-ops-* or grab-ops-spike-* in the login folder, in plain characters: made here when it
// isn't there, and every name this case makes in it starts with "ops-", which 'cleanup' removes
// (with the folder) and nothing else. 'readonly' tries every change on new random names
// (.grab-ops-probe-*) in the folders it is given, only where writes are allowed anyway (the rule
// above) or statvfs says the file system is read-only, and removes at once anything it did make. Only
// codes, numbers and yes/no are printed (a refusal's own short text too): no name but the folders
// given, an owner's or group's name only as its length. Lines start with "hook: ".
//
//   GRAB_TEST_SERVER          the server's name in grab.conf
//   GRAB_TEST_APPDATA         the %APPDATA% the programs use (a copy of the settings); grab.conf there
//   GRAB_TEST_OPS             some of caps,names,links,copy,tree,remove,readonly,cleanup (comma-separated),
//                             run in that order
//   GRAB_TEST_OPS_DIR         the folder the writes go to (grab's form: /tmp/grab-ops-<x> or grab-ops-spike-<x>)
//   copy: a source file of GRAB_TEST_OPS_SOURCE_MIB MiB (256), written over 4 connections (kept for the
//         next run), then a copy of each of GRAB_TEST_OPS_MIB's sizes (MiB, comma-separated, 256; a
//         bigger one, a multiple of the source, first built from copies of it at its offsets) by
//         copy_range in pieces of at most GRAB_TEST_OPS_PIECE_MIB MiB (256), sized to the server's pace
//         within the connection's timeout, each checked by the server's md5sum
//   tree: GRAB_TEST_OPS_FILES small files (2000) in 20 folders and an empty chain of 4, then copied by
//         copy-data with their times set, GRAB_TEST_OPS_IN_FLIGHT files at a time (32) on one connection
//   remove: every ops-tree-* folder: its files removed pipelined (twice GRAB_TEST_OPS_IN_FLIGHT at a
//         time), its folders deepest first
//   readonly: in each of GRAB_TEST_OPS_RO_DIR's folders ('|' between, "." the login folder; required)
//
// At most 4 logins at once.

#include "live_settings.hpp"
#include "live_tools.hpp"
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
#include <expected>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace grab;
using namespace grab::test::live;

namespace {

// The folder the writes may go to: /tmp/grab-ops-<x> or grab-ops-spike-<x> in the login folder,
// <x> in plain characters (letters, digits, '.', '-', '_'), nothing below it.
bool ops_dir_allowed(std::string_view dir) {
    constexpr std::string_view tmp = "/tmp/";
    const bool in_tmp = dir.starts_with(tmp);
    const std::string_view name = in_tmp ? dir.substr(tmp.size()) : dir;
    const std::string_view prefix = in_tmp ? "grab-ops-" : "grab-ops-spike-";
    return name.starts_with(prefix) && name.size() > prefix.size() && std::ranges::all_of(name, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '-' || c == '_';
           });
}

struct Dir {
    std::string dir; // GRAB_TEST_OPS_DIR, grab's form ("" when not given)
    std::string raw; // the same as raw bytes
};

// GRAB_TEST_OPS_DIR there as a folder: made when missing. false: it can't be used.
bool ensure_dir(sftp::Client& s, const Dir& d) {
    auto there = sftp::lstat(s, d.raw);
    if (there && there->is_dir()) {
        say(std::format("folder {}: there already", d.dir));
        return true;
    }
    if (there) {
        say(std::format("folder {}: there, but not a folder: nothing written", d.dir));
        return false;
    }
    auto made = sftp::mkdir(s, d.raw, sftp::Attrs{.size = {}, .permissions = 0700, .mtime = {}, .uid = {}, .gid = {}, .atime = {}});
    say(std::format("folder {}: MKDIR {}", d.dir, code_of(made)));
    return made.has_value();
}

// Jobs 0..n-1 on one connection with at most `width` of them going at once: start(i, finish)
// queues job i's first requests, their callbacks queue the next ones, and its last one calls
// finish() (exactly once).
void run_window(sftp::Client& s, std::size_t n, std::size_t width,
                const std::function<void(std::size_t, const std::function<void()>&)>& start) {
    std::size_t next = 0;
    std::size_t going = 0;
    const std::function<void()> finish = [&going] { --going; };
    while (next < n || going > 0) {
        while (going < std::max<std::size_t>(width, 1) && next < n) {
            ++going;
            start(next++, finish);
        }
        if (going > 0) s.pump();
    }
}

// What a run of jobs met: how many failed, and the first failure's code.
struct Failures {
    std::size_t count = 0;
    std::string first;
    void add(const sftp::Status& st) {
        if (count++ == 0) first = code_of(std::expected<void, sftp::Status>(std::unexpected(st)));
    }
    [[nodiscard]] std::string shown() const {
        return count == 0 ? "failed 0" : std::format("FAILED {} (the first: {})", count, first);
    }
};

// ---- caps, names, links ------------------------------------------------------------------------

void caps(sftp::Client& s, const Dir& d, const ssh::Target& t) {
    say(std::format("caps: SFTP version {}; copy-data offered {}; users-groups-by-id offered {}; statvfs offered {}; limits offered {}; "
                    "the connection's timeout {} s",
                    s.version(), yes(s.has_extension(sftp::copy_data_extension)), yes(s.has_extension(sftp::users_groups_extension)),
                    yes(s.has_extension("statvfs@openssh.com")), yes(s.has_extension("limits@openssh.com")),
                    std::chrono::duration_cast<std::chrono::seconds>(t.timeout).count()));
    if (!s.has_extension("statvfs@openssh.com")) return;
    std::vector<std::pair<std::string, std::string>> where{{".", "."}};
    if (!d.dir.empty()) where.emplace_back(d.dir, d.raw);
    for (const auto& [label, raw] : where) {
        auto v = sftp::statvfs(s, raw);
        if (v) say(std::format("caps: statvfs({}): {} free; read-only flag {}", label, util::format_size(v->free_bytes()), v->read_only() ? "yes" : "no"));
        else say(std::format("caps: statvfs({}): {}", label, code_of(v)));
    }
}

// The names of the owner and group of the folder (made by this login): only whether they came and
// their lengths; and of a number nobody has.
void names(sftp::Client& s, const Dir& d) {
    auto a = sftp::stat(s, d.raw.empty() ? std::string(".") : d.raw);
    if (!a) {
        say("names: STAT " + code_of(a));
        return;
    }
    if (!a->uid || !a->gid) {
        say("names: the server sent no owner and group");
        return;
    }
    if (!s.has_extension(sftp::users_groups_extension)) {
        say("names: users-groups-by-id@openssh.com not offered");
        return;
    }
    constexpr std::uint32_t nobody = 2'147'483'646;
    const std::array<std::uint32_t, 2> uids{*a->uid, nobody};
    const std::array<std::uint32_t, 2> gids{*a->gid, nobody};
    auto n = sftp::users_groups_by_id(s, uids, gids);
    if (!n) {
        say("names: users-groups-by-id " + code_of(n));
        return;
    }
    say(std::format("names: owner and group sent yes; the owner resolved {} ({} characters); the group resolved {} ({} characters); "
                    "a number nobody has empty: {}, {}",
                    yes(!n->users[0].empty()), n->users[0].size(), yes(!n->groups[0].empty()), n->groups[0].size(),
                    yes(n->users[1].empty()), yes(n->groups[1].empty())));
}

// SYMLINK and READLINK: the order of SYMLINK's paths (OpenSSH's: the target first), a relative
// target, one pointing nowhere, the refusals; then everything made removed.
void links(sftp::Client& s, const Dir& d) {
    const std::string base = "ops-link-" + token();
    const std::string target = in_dir(d.dir, base + "-target");
    const std::string link = in_dir(d.dir, base);
    const std::string relative = in_dir(d.dir, base + "-rel");
    const std::string dangling = in_dir(d.dir, base + "-dangling");
    const std::string nowhere = in_dir(d.dir, base + "-none");
    if (auto made = create_file(s, target, "target"); !made) {
        say("links: the target file couldn't be made: " + code_of(made));
        return;
    }
    const auto untouched = [&] {
        auto a = sftp::lstat(s, target);
        return a && a->is_file() && a->size == 6u;
    };
    const auto points_to = [&](const std::string& at, std::string_view want) {
        auto read = sftp::readlink(s, at);
        return read && *read == want;
    };
    auto made = sftp::symlink(s, link, target);
    bool openssh_order = false;
    if (made) {
        auto as_link = sftp::lstat(s, link);
        auto through = sftp::stat(s, link);
        const bool where = as_link && as_link->is_link();
        const bool reads = points_to(link, target);
        openssh_order = where && reads && untouched();
        say(std::format("links: SYMLINK {}; a link where asked: {}; READLINK gives the target: {}; STAT follows it: {}; the target "
                        "untouched: {} (OpenSSH's order: {})",
                        code_of(made), yes(where), yes(reads), yes(through && through->size == 6u), yes(untouched()), yes(openssh_order)));
    } else {
        auto at_link = sftp::lstat(s, link);
        say(std::format("links: SYMLINK {}: no link made (the link's name free: {}; the target untouched: {})", code_of(made),
                        yes(!at_link), yes(untouched())));
    }
    // A relative target only once the order is known: a server taking the draft's order would
    // make a link at the relative path, which is from the login folder, outside GRAB_TEST_OPS_DIR.
    if (openssh_order) {
        const std::string name = base + "-target";
        auto rel = sftp::symlink(s, relative, name);
        auto through = sftp::stat(s, relative);
        say(std::format("links: relative: SYMLINK {}; READLINK as given: {}; STAT follows it from the link's folder: {}", code_of(rel),
                        yes(rel && points_to(relative, name)), yes(through && through->size == 6u)));
        auto lost = sftp::symlink(s, dangling, nowhere);
        auto missing = sftp::stat(s, dangling);
        say(std::format("links: pointing nowhere: SYMLINK {}; READLINK as given: {}; STAT {}", code_of(lost),
                        yes(lost && points_to(dangling, nowhere)), code_of(missing)));
        auto onto = sftp::symlink(s, target, link);
        say(std::format("links: onto an existing name: SYMLINK {}; that file untouched: {}", code_of(onto), yes(untouched())));
    }
    auto of_file = sftp::readlink(s, target);
    auto of_missing = sftp::readlink(s, nowhere);
    say(std::format("links: READLINK of a file {}; of a missing name {}", code_of(of_file), code_of(of_missing)));
    // Each link removed as itself (never its target), then the target.
    std::string removed;
    for (const std::string& l : {link, relative, dangling}) {
        if (!sftp::lstat(s, l)) continue;
        removed += std::format("{}{}", removed.empty() ? "" : ", ", code_of(sftp::remove(s, l)));
    }
    const bool kept = untouched();
    auto gone_target = sftp::remove(s, target);
    bool clean = true;
    for (const std::string& f : {target, link, relative, dangling, nowhere}) clean = clean && !sftp::lstat(s, f);
    say(std::format("links: REMOVE of each link {}; the target still there after: {}; REMOVE of the target {}; everything it made "
                    "removed: {}",
                    removed.empty() ? "none made" : removed, yes(kept), code_of(gone_target), yes(clean)));
}

// ---- copy ---------------------------------------------------------------------------------------

// MD5 of the first `size` bytes of `data`, `times` times over, as md5sum prints it.
std::string repeated_md5(const Data& data, std::uint64_t size, std::uint64_t times) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    REQUIRE(ctx != nullptr);
    REQUIRE(EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) == 1);
    std::vector<char> buf(mib);
    for (std::uint64_t k = 0; k < times; ++k) {
        for (std::uint64_t at = 0; at < size; at += buf.size()) {
            const auto n = static_cast<std::size_t>(std::min<std::uint64_t>(buf.size(), size - at));
            data.fill(at, std::span(buf.data(), n));
            REQUIRE(EVP_DigestUpdate(ctx, buf.data(), n) == 1);
        }
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int len = 0;
    REQUIRE(EVP_DigestFinal_ex(ctx, digest.data(), &len) == 1);
    EVP_MD_CTX_free(ctx);
    std::string out;
    for (unsigned int i = 0; i < len; ++i) out += std::format("{:02x}", digest[i]);
    return out;
}

// The source file, `size` bytes of `data`: written over 4 connections unless an earlier run left it
// with that size. Its raw path, or none.
std::optional<std::string> source_file(ssh::Pool& pool, const Dir& d, std::uint64_t size, const Data& data) {
    const std::string raw = in_dir(d.dir, "ops-src.bin");
    {
        auto lease = pool.acquire({}, ssh::interactive_retry);
        REQUIRE(lease);
        auto& s = (*lease)->sftp();
        auto there = sftp::lstat(s, raw);
        if (there && there->is_file() && there->size == size) {
            say(std::format("copy: the source: {} MiB from an earlier run", size / mib));
            return raw;
        }
        if (there) {
            if (auto removed = sftp::remove(s, raw); !removed) {
                say("copy: an earlier source couldn't be removed: " + code_of(removed));
                return std::nullopt;
            }
        }
        if (auto made = create_file(s, raw, ""); !made) {
            say("copy: the source couldn't be made: " + code_of(made));
            return std::nullopt;
        }
    }
    const auto [parts, took] = write_parallel(pool, raw, size, 4, data, 64, sftp::max_unacked, sftp::max_write);
    std::string failed;
    for (const Part& p : parts)
        if (!p.failed.empty()) failed += (failed.empty() ? "" : "; ") + p.failed;
    if (!failed.empty()) {
        say("copy: the source couldn't be written: " + failed);
        return std::nullopt;
    }
    say(std::format("copy: the source: {} MiB written over 4 connections in {:.1f} s", size / mib, took));
    return raw;
}

// Two files' handles: `from` open for reading, `to` new (exclusive) for writing; empty: not open.
struct Pair {
    std::string from;
    std::string to;
};

// Both opened into `p`; when one fails, what did open stays there for close_pair.
std::expected<void, std::string> open_pair(sftp::Client& s, Pair& p, const std::string& from_raw, const std::string& to_raw) {
    auto from = sftp::open_file(s, from_raw);
    if (!from) return std::unexpected("OPEN of the source " + code_of(from));
    p.from = *from;
    auto to = sftp::open_file(s, to_raw, sftp::OpenFlags::write | sftp::OpenFlags::creat | sftp::OpenFlags::excl);
    if (!to) return std::unexpected("OPEN of the copy " + code_of(to));
    p.to = *to;
    return {};
}

void close_pair(sftp::Client& s, const Pair& p) {
    for (const std::string* h : {&p.from, &p.to}) {
        if (h->empty()) continue;
        if (auto closed = sftp::close_file(s, *h); !closed) {
            // The copy's outcome is what was looked at; a handle that won't close changes nothing.
        }
    }
}

// `big` made of `times` copies of the source at its offsets (copy_range's to_offset): why not, or
// nothing.
std::optional<std::string> build(sftp::Client& s, const std::string& source, std::uint64_t source_size, const std::string& big,
                                 std::uint64_t times, std::chrono::milliseconds budget, std::uint64_t piece) {
    Pair p;
    std::optional<std::string> failed;
    if (auto opened = open_pair(s, p, source, big); !opened) failed = opened.error();
    const auto t0 = Clock::now();
    for (std::uint64_t k = 0; k < times && !failed; ++k) {
        if (auto r = sftp::copy_range(s, p.from, 0, source_size, p.to, k * source_size, budget, {}, {}, piece); !r)
            failed = std::format("copy {} of {}: {}", k + 1, times, code_of(r));
    }
    const double took = seconds(Clock::now() - t0);
    close_pair(s, p);
    if (!failed) {
        const std::uint64_t size = source_size * times;
        say(std::format("copy: built {} MiB from {} copies of the source at its offsets in {:.1f} s = {:.1f} MiB/s", size / mib, times,
                        took, took > 0 ? static_cast<double>(size) / mib / took : 0.0));
    }
    return failed;
}

// `from` copied into the new file `copy` in pieces (sized to the pace within `budget`, the
// connection's timeout), each timed; its size and the server's md5sum (of `times` copies of the
// source's bytes) checked.
void copy_timed(ssh::Pool& pool, sftp::Client& s, const std::string& from, const std::string& copy, const Data& data,
                std::uint64_t source_size, std::uint64_t times, std::chrono::milliseconds budget, std::uint64_t piece) {
    const std::uint64_t size = source_size * times;
    Pair p;
    if (auto opened = open_pair(s, p, from, copy); !opened) {
        close_pair(s, p);
        say(std::format("copy: {} MiB: {}", size / mib, opened.error()));
        return;
    }
    std::vector<double> pieces;
    std::vector<std::uint64_t> lengths;
    auto last = Clock::now();
    const auto t0 = last;
    std::uint64_t before = 0;
    auto r = sftp::copy_range(
        s, p.from, 0, size, p.to, 0, budget,
        [&](std::uint64_t done) {
            const auto now = Clock::now();
            pieces.push_back(seconds(now - last));
            lengths.push_back(done - before);
            last = now;
            before = done;
        },
        {}, piece);
    const double took = seconds(Clock::now() - t0);
    close_pair(s, p);
    if (!r) {
        say(std::format("copy: {} MiB: FAILED after {:.1f} s ({} piece(s) done): {}", size / mib, took, pieces.size(), code_of(r)));
        return;
    }
    auto attrs = sftp::stat(s, copy);
    const auto server = server_md5(pool, copy);
    const std::string md5 = !server ? "unavailable (" + server.error() + ")"
                            : *server == repeated_md5(data, source_size, times) ? "equal"
                                                                               : "DIFFERENT";
    say(std::format("copy: {} MiB in pieces of {} MiB: {} piece(s) (the first {:.1f} MiB, the largest {:.1f} MiB) in {:.2f} s = "
                    "{:.1f} MiB/s (slowest piece {:.2f} s, fastest {:.2f} s); size right: {}; server md5sum {}",
                    size / mib, piece / mib, pieces.size(), lengths.empty() ? 0.0 : static_cast<double>(lengths.front()) / mib,
                    lengths.empty() ? 0.0 : static_cast<double>(std::ranges::max(lengths)) / mib, took,
                    took > 0 ? static_cast<double>(size) / mib / took : 0.0, pieces.empty() ? 0.0 : std::ranges::max(pieces),
                    pieces.empty() ? 0.0 : std::ranges::min(pieces), yes(attrs && attrs->size == size), md5));
}

// A copy of `size` bytes (a multiple of the source's: a bigger one is built first), checked by the
// server's md5sum; what it made removed again.
void copy_one(ssh::Pool& pool, const Dir& d, const std::string& source, std::uint64_t source_size, const Data& data,
              std::uint64_t size, std::chrono::milliseconds budget, std::uint64_t piece) {
    if (source_size == 0 || size == 0 || size % source_size != 0) {
        say(std::format("copy: {} MiB isn't a multiple of the source's {} MiB: not tried", size / mib, source_size / mib));
        return;
    }
    const std::uint64_t times = size / source_size;
    const std::string tok = token();
    const std::string big = in_dir(d.dir, std::format("ops-big-{}-{}.bin", size / mib, tok));
    const std::string copy = in_dir(d.dir, std::format("ops-copy-{}-{}.bin", size / mib, tok));
    auto lease = pool.acquire({}, ssh::interactive_retry);
    REQUIRE(lease);
    const auto started = Clock::now();
    try {
        auto& s = (*lease)->sftp();
        const std::optional<std::string> failed = times > 1 ? build(s, source, source_size, big, times, budget, piece) : std::nullopt;
        if (failed) say(std::format("copy: building {} MiB: {}", size / mib, *failed));
        else copy_timed(pool, s, times > 1 ? big : source, copy, data, source_size, times, budget, piece);
    } catch (const sftp::Broken& e) {
        // A piece longer than the connection's timeout ends here.
        lease->broken();
        say(std::format("copy: {} MiB: the connection broke after {:.1f} s: {}", size / mib, seconds(Clock::now() - started), e.what()));
        lease.reset();
        lease = pool.acquire({}, ssh::interactive_retry);
        REQUIRE(lease);
    }
    auto& t = (*lease)->sftp();
    std::string removed;
    for (const std::string& f : {copy, big}) {
        if (!sftp::lstat(t, f)) continue;
        removed += std::format("{}{}", removed.empty() ? "" : ", ", code_of(sftp::remove(t, f)));
    }
    say(std::format("copy: {} MiB: REMOVE of what it made {}", size / mib, removed.empty() ? "none made" : removed));
}

// ---- tree, remove --------------------------------------------------------------------------------

// The tree's layout: its folders, parents first (20 that hold the files, and an empty chain), and
// its files, file i in folder i % 20, of i * 7919 % 16384 + 1 bytes (every 500th empty).
constexpr std::size_t tree_folders_holding = 20;

std::vector<std::string> tree_folders() {
    std::vector<std::string> out;
    for (std::size_t i = 0; i < tree_folders_holding; ++i) out.push_back(std::format("d{:02}", i));
    for (const char* chain : {"deep", "deep/a", "deep/a/b", "deep/a/b/c"}) out.emplace_back(chain);
    return out;
}

struct TreeFile {
    std::string rel;
    std::uint64_t size = 0;
};

std::vector<TreeFile> tree_files(std::size_t n) {
    std::vector<TreeFile> out;
    for (std::size_t i = 0; i < n; ++i)
        out.push_back(TreeFile{.rel = std::format("d{:02}/f{:04}.bin", i % tree_folders_holding, i),
                               .size = i % 500 == 0 ? 0 : (i * 7919) % 16384 + 1});
    return out;
}

// File i's bytes.
std::string tree_bytes(const Data& data, std::size_t i, std::uint64_t size) {
    std::string out(static_cast<std::size_t>(size), '\0');
    data.fill(std::uint64_t{i} * 16384, std::span(out.data(), out.size()));
    return out;
}

std::string under(const std::string& root, std::string_view rel) { return rel.empty() ? root : root + "/" + std::string(rel); }

std::size_t depth_of(std::string_view rel) { return static_cast<std::size_t>(std::ranges::count(rel, '/')); }

// A folder's subtree (relative paths), never through a link: each folder is LSTATed before it is
// listed, as a server's READDIR may follow links (OpenSSH's doesn't).
struct Walked {
    std::vector<std::string> folders;                       // by path, so parents first
    std::vector<std::pair<std::string, sftp::Attrs>> files; // by path, with their attributes
    std::vector<std::string> others;                        // links and the rest: removed as names
    std::optional<sftp::Status> failed;
};

Walked walk(sftp::Client& s, const std::string& root) {
    Walked w;
    std::vector<std::string> queue{""};
    for (std::size_t at = 0; at < queue.size() && !w.failed; ++at) {
        const std::string rel = queue[at];
        if (!rel.empty()) {
            auto self = sftp::lstat(s, under(root, rel));
            if (!self || !self->is_dir()) {
                w.others.push_back(rel);
                continue;
            }
            w.folders.push_back(rel);
        }
        auto listed = sftp::list(s, under(root, rel));
        if (!listed) {
            w.failed = listed.error();
            break;
        }
        for (auto& e : *listed) {
            const std::string child = rel.empty() ? e.name : rel + "/" + e.name;
            if (e.attrs.is_dir()) queue.push_back(child);
            else if (e.attrs.is_file()) w.files.emplace_back(child, e.attrs);
            else w.others.push_back(child);
        }
    }
    // The same order whatever order the server listed them in.
    std::ranges::sort(w.folders);
    std::ranges::sort(w.files, {}, &std::pair<std::string, sftp::Attrs>::first);
    return w;
}

// MKDIR of `folders` (relative, parents first) under `root`, one depth at a time, pipelined.
Failures make_folders(sftp::Client& s, const std::string& root, const std::vector<std::string>& folders, std::size_t width) {
    Failures f;
    std::map<std::size_t, std::vector<std::string>> by_depth;
    for (const std::string& rel : folders) by_depth[depth_of(rel)].push_back(rel);
    for (const auto& [depth, level] : by_depth) {
        run_window(s, level.size(), width, [&](std::size_t i, const std::function<void()>& finish) {
            s.mkdir(under(root, level[i]), {}, [&f, finish](std::expected<void, sftp::Status> r) {
                if (!r) f.add(r.error());
                finish();
            });
        });
    }
    return f;
}

// The tree under a new folder `root`: its folders, then its files (OPEN new, WRITE, CLOSE),
// `width` files at a time.
Failures make_tree(sftp::Client& s, const std::string& root, const std::vector<TreeFile>& files, const Data& data, std::size_t width) {
    Failures f;
    if (auto made = sftp::mkdir(s, root); !made) {
        f.add(made.error());
        return f;
    }
    Failures folders = make_folders(s, root, tree_folders(), width);
    if (folders.count > 0) return folders;
    run_window(s, files.size(), width, [&](std::size_t i, const std::function<void()>& finish) {
        s.open(under(root, files[i].rel), sftp::OpenFlags::write | sftp::OpenFlags::creat | sftp::OpenFlags::excl, {},
               [&s, &f, &files, &data, i, finish](std::expected<std::string, sftp::Status> h) {
                   if (!h) {
                       f.add(h.error());
                       return finish();
                   }
                   // Done once every reply came, whatever their order.
                   auto left = std::make_shared<int>(files[i].size > 0 ? 2 : 1);
                   const auto answered = [&f, left, finish](std::expected<void, sftp::Status> r) {
                       if (!r) f.add(r.error());
                       if (--*left == 0) finish();
                   };
                   if (files[i].size > 0) s.write(*h, 0, tree_bytes(data, i, files[i].size), answered);
                   s.close(*h, answered);
               });
    });
    return f;
}

// One file's copy by copy-data: both files open, the bytes copied (all of them: length 0), the
// copy's time set to the source's, both closed; done once every reply came, whatever their order.
struct FileCopy {
    std::optional<std::expected<std::string, sftp::Status>> from, to;
    int left = 0; // replies still to come
};

void copy_file(sftp::Client& s, const std::string& from, const std::string& to, std::uint32_t mtime, Failures& f,
               const std::function<void()>& finish) {
    auto j = std::make_shared<FileCopy>();
    const auto answered = [&f, j, finish](std::expected<void, sftp::Status> r) {
        if (!r) f.add(r.error());
        if (--j->left == 0) finish();
    };
    const auto close_all = [&s, j, finish, answered] {
        for (const auto* h : {&j->from, &j->to}) {
            if (!*h || !**h) continue;
            ++j->left;
            s.close(***h, answered);
        }
        if (j->left == 0) finish();
    };
    const auto both_open = [&s, &f, j, mtime, answered, close_all] {
        if (!j->from || !j->to) return; // the other's answer is still coming
        if (!*j->from || !*j->to) {
            f.add(!*j->from ? j->from->error() : j->to->error());
            return close_all();
        }
        j->left += 2;
        s.copy_data(**j->from, 0, 0, **j->to, 0, answered);
        const sftp::Attrs times{.size = {}, .permissions = {}, .mtime = mtime, .uid = {}, .gid = {}, .atime = {}};
        s.fsetstat(**j->to, times, answered);
        close_all();
    };
    s.open(from, [j, both_open](std::expected<std::string, sftp::Status> h) {
        j->from = std::move(h);
        both_open();
    });
    s.open(to, sftp::OpenFlags::write | sftp::OpenFlags::creat | sftp::OpenFlags::excl, {},
           [j, both_open](std::expected<std::string, sftp::Status> h) {
               j->to = std::move(h);
               both_open();
           });
}

void tree(ssh::Pool& pool, const Dir& d, std::size_t n, std::size_t width) {
    auto lease = pool.acquire({}, ssh::interactive_retry);
    REQUIRE(lease);
    auto& s = (*lease)->sftp();
    if (!s.has_extension(sftp::copy_data_extension)) {
        say("tree: copy-data not offered: not tried");
        return;
    }
    const std::string root = in_dir(d.dir, "ops-tree-" + token());
    const std::string copy = root + "-copy";
    const Data data(0x7EE);
    const auto files = tree_files(n);
    std::uint64_t bytes = 0;
    for (const TreeFile& f : files) bytes += f.size;
    auto t0 = Clock::now();
    const Failures made = make_tree(s, root, files, data, width);
    double took = seconds(Clock::now() - t0);
    say(std::format("tree: made {} files ({:.1f} MiB) in {} folders in {:.1f} s = {:.1f} files/s; {}", files.size(),
                    static_cast<double>(bytes) / mib, tree_folders().size(), took, took > 0 ? static_cast<double>(files.size()) / took : 0.0,
                    made.shown()));
    if (made.count > 0) return;

    // The copy as the window will make it: the source walked, its folders made, its files copied
    // with their times.
    t0 = Clock::now();
    const Walked source = walk(s, root);
    const double walked = seconds(Clock::now() - t0);
    if (source.failed) {
        say("tree copy: the walk " + code_of(std::expected<void, sftp::Status>(std::unexpected(*source.failed))));
        return;
    }
    t0 = Clock::now();
    Failures failed;
    if (auto top = sftp::mkdir(s, copy); !top) failed.add(top.error());
    else failed = make_folders(s, copy, source.folders, width);
    const double folders_took = seconds(Clock::now() - t0);
    std::vector<std::pair<std::string, std::uint32_t>> todo;
    for (const auto& [rel, attrs] : source.files) todo.emplace_back(rel, attrs.mtime.value_or(0));
    t0 = Clock::now();
    if (failed.count == 0) {
        run_window(s, todo.size(), width, [&](std::size_t i, const std::function<void()>& finish) {
            copy_file(s, under(root, todo[i].first), under(copy, todo[i].first), todo[i].second, failed, finish);
        });
    }
    took = seconds(Clock::now() - t0);

    // The same names, sizes and times; a few files read back.
    const Walked copied = walk(s, copy);
    bool equal = !copied.failed && copied.folders == source.folders && copied.files.size() == source.files.size();
    for (std::size_t i = 0; equal && i < source.files.size(); ++i) {
        const auto& [rel, attrs] = source.files[i];
        const auto& [copied_rel, copied_attrs] = copied.files[i];
        equal = copied_rel == rel && copied_attrs.size == attrs.size && copied_attrs.mtime == attrs.mtime;
    }
    bool read_back = true;
    for (const std::size_t i : {std::size_t{1}, n / 4, n / 2, n - 1, n / 3}) {
        if (i >= n) continue;
        auto got = read_small(s, under(copy, files[i].rel));
        read_back = read_back && got && *got == tree_bytes(data, i, files[i].size);
    }
    say(std::format("tree copy: {} files in {} folders: walked in {:.1f} s, folders made in {:.1f} s, files copied in {:.1f} s = "
                    "{:.1f} files/s ({:.2f} MiB/s), {} at a time; {}; names, sizes and times equal: {}; 5 read back equal: {}",
                    todo.size(), source.folders.size(), walked, folders_took, took,
                    took > 0 ? static_cast<double>(todo.size()) / took : 0.0, took > 0 ? static_cast<double>(bytes) / mib / took : 0.0,
                    width, failed.shown(), yes(equal), yes(read_back)));
}

// Everything at and below `root` (a real folder: not a link), files pipelined, folders deepest
// first. The counts, and how long each part took.
struct Removal {
    std::size_t files = 0, folders = 0, left = 0;
    double walked = 0, files_took = 0, folders_took = 0;
    Failures failed;
};

Removal remove_tree(sftp::Client& s, const std::string& root, std::size_t width) {
    Removal r;
    auto self = sftp::lstat(s, root);
    if (!self || !self->is_dir()) {
        ++r.left;
        return r;
    }
    auto t0 = Clock::now();
    const Walked w = walk(s, root);
    r.walked = seconds(Clock::now() - t0);
    if (w.failed) r.failed.add(*w.failed);
    std::vector<std::string> names;
    for (const auto& [rel, attrs] : w.files) names.push_back(rel);
    names.insert(names.end(), w.others.begin(), w.others.end()); // links: the link itself goes
    t0 = Clock::now();
    run_window(s, names.size(), width, [&](std::size_t i, const std::function<void()>& finish) {
        s.remove(under(root, names[i]), [&r, finish](std::expected<void, sftp::Status> done) {
            if (done) ++r.files;
            else r.failed.add(done.error());
            finish();
        });
    });
    r.files_took = seconds(Clock::now() - t0);
    t0 = Clock::now();
    std::map<std::size_t, std::vector<std::string>, std::greater<>> by_depth;
    for (const std::string& rel : w.folders) by_depth[depth_of(rel)].push_back(rel);
    for (const auto& [depth, level] : by_depth) {
        run_window(s, level.size(), width, [&](std::size_t i, const std::function<void()>& finish) {
            s.rmdir(under(root, level[i]), [&r, finish](std::expected<void, sftp::Status> done) {
                if (done) ++r.folders;
                else r.failed.add(done.error());
                finish();
            });
        });
    }
    if (auto top = sftp::rmdir(s, root)) ++r.folders;
    else r.failed.add(top.error());
    r.folders_took = seconds(Clock::now() - t0);
    r.left = r.failed.count;
    return r;
}

// Every ops-tree-* folder in the folder removed, timed.
void remove_trees(sftp::Client& s, const Dir& d, std::size_t width) {
    auto listed = sftp::list(s, d.raw);
    if (!listed) {
        say("remove: the folder's listing " + code_of(listed));
        return;
    }
    std::size_t trees = 0;
    for (const auto& e : *listed) {
        if (!e.name.starts_with("ops-tree-") || !e.attrs.is_dir()) continue;
        ++trees;
        const std::string root = d.raw + "/" + e.name;
        const Removal r = remove_tree(s, root, width);
        say(std::format("remove: a tree of {} files and {} folders: walked in {:.1f} s; files removed in {:.1f} s = {:.1f} files/s ({} at "
                        "a time); folders RMDIR deepest first in {:.2f} s; {}; gone: {}",
                        r.files, r.folders, r.walked, r.files_took, r.files_took > 0 ? static_cast<double>(r.files) / r.files_took : 0.0,
                        width, r.folders_took, r.failed.shown(), yes(!sftp::lstat(s, root))));
    }
    if (trees == 0) say("remove: no ops-tree-* folder there");
}

// ---- readonly, cleanup ---------------------------------------------------------------------------

// Every change tried in `dir` on new random names, as on a server this account may only read
// (Box.Guest): each answer's code, and that nothing was made (anything that was is removed at once).
// Only in a folder writes may go to anyway (ops_dir_allowed: Main's chmod 555 /tmp/grab-ops-ro-*)
// or on a file system statvfs says is read-only; elsewhere nothing is tried. true: tried, nothing
// made.
bool readonly(sftp::Client& s, std::string_view dir) {
    const std::string base = ".grab-ops-probe-" + token();
    const std::string folder = in_dir(dir, base + ".d");
    const std::string file = in_dir(dir, base);
    const std::string a = in_dir(dir, base + "-a");
    const std::string b = in_dir(dir, base + "-b");
    const std::string link = in_dir(dir, base + "-link");
    std::string line = std::format("readonly {}:", dir);
    bool read_only = false;
    if (s.has_extension("statvfs@openssh.com")) {
        auto v = sftp::statvfs(s, dir.empty() ? std::string(".") : rclone_enc::raw_path(dir));
        read_only = v && v->read_only();
        line += v ? std::format(" statvfs read-only {};", v->read_only() ? "yes" : "no") : std::format(" statvfs {};", code_of(v));
    }
    if (!read_only && !ops_dir_allowed(dir)) {
        say(line + " not probed: not read-only (and not a folder writes may go to): nothing tried");
        return false;
    }
    std::string made_anyway;
    auto mk = sftp::mkdir(s, folder);
    line += std::format(" MKDIR {};", code_of(mk));
    if (mk) made_anyway += std::format(" a folder (RMDIR {})", code_of(sftp::rmdir(s, folder)));
    auto h = sftp::open_file(s, file, sftp::OpenFlags::write | sftp::OpenFlags::creat | sftp::OpenFlags::excl);
    line += std::format(" OPEN (create, exclusive: what a copy writes into) {};", code_of(h));
    if (h) {
        auto closed = sftp::close_file(s, *h);
        made_anyway += std::format(" a file (CLOSE {}, REMOVE {})", code_of(closed), code_of(sftp::remove(s, file)));
    }
    line += std::format(" RENAME of a missing name {};", code_of(sftp::rename(s, a, b)));
    line += std::format(" REMOVE of a missing name {};", code_of(sftp::remove(s, a)));
    auto ln = sftp::symlink(s, link, base + "-nowhere");
    line += std::format(" SYMLINK {};", code_of(ln));
    if (ln) made_anyway += std::format(" a link (REMOVE {})", code_of(sftp::remove(s, link)));
    line += h ? " copy-data: not tried (the file could be made: this server writes here);"
              : " copy-data: no file to copy into (OPEN refused);";
    bool nothing = true;
    for (const std::string& f : {folder, file, a, b, link}) nothing = nothing && !sftp::lstat(s, f);
    line += std::format(" nothing made: {}", made_anyway.empty() ? "yes" : "NO, made and removed again:" + made_anyway);
    line += std::format("; nothing left: {}", yes(nothing));
    say(line);
    return nothing && made_anyway.empty();
}

// The folder's own entries (ops-*), whatever is in them, then the folder itself.
void cleanup(sftp::Client& s, const Dir& d, std::size_t width) {
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
    std::size_t files = 0, folders = 0, left = 0;
    for (const auto& e : *listed) {
        const std::string path = d.raw + "/" + e.name;
        if (!e.name.starts_with("ops-")) {
            ++left;
            continue;
        }
        if (e.attrs.is_dir()) {
            const Removal r = remove_tree(s, path, width);
            files += r.files;
            folders += r.folders;
            left += r.left;
        } else if (sftp::remove(s, path)) {
            ++files;
        } else {
            ++left;
        }
    }
    auto removed = sftp::rmdir(s, d.raw);
    const bool gone = !sftp::lstat(s, d.raw);
    say(std::format("cleanup: {} file(s) and {} folder(s) removed, {} left; RMDIR of the folder {}; gone: {}", files, folders, left,
                    code_of(removed), yes(gone)));
}

} // namespace

TEST_CASE("live server: server file operations, printed only (GRAB_TEST_OPS)") {
    const auto name = util::getenv_utf8("GRAB_TEST_SERVER");
    const auto appdata = util::getenv_utf8("GRAB_TEST_APPDATA");
    const auto modes = util::getenv_utf8("GRAB_TEST_OPS");
    if (!name || !appdata || !modes) {
        MESSAGE("skipped: GRAB_TEST_SERVER / GRAB_TEST_APPDATA / GRAB_TEST_OPS not set");
        return;
    }
    const auto list = util::split(*modes, ',');
    const auto wanted = [&](std::string_view mode) { return std::ranges::find(list, mode) != list.end(); };
    Dir d;
    d.dir = util::getenv_utf8("GRAB_TEST_OPS_DIR").value_or("");
    const bool writes = wanted("links") || wanted("copy") || wanted("tree") || wanted("remove") || wanted("cleanup");
    if (!d.dir.empty() || writes) {
        const std::string why = "GRAB_TEST_OPS_DIR must be /tmp/grab-ops-* or grab-ops-spike-* (got \"" + d.dir + "\")";
        REQUIRE_MESSAGE(ops_dir_allowed(d.dir), why);
        d.raw = rclone_enc::raw_path(d.dir);
    }
    std::vector<std::string> ro_dirs;
    for (const auto& p : util::split(util::getenv_utf8("GRAB_TEST_OPS_RO_DIR").value_or(""), '|'))
        if (!p.empty()) ro_dirs.emplace_back(p);
    const bool readonly_has_folders = !wanted("readonly") || !ro_dirs.empty();
    REQUIRE_MESSAGE(readonly_has_folders, "'readonly' needs GRAB_TEST_OPS_RO_DIR");
    const std::uint64_t source_size = number_from_env("GRAB_TEST_OPS_SOURCE_MIB", 256) * mib;
    const std::uint64_t piece = std::max<std::uint64_t>(number_from_env("GRAB_TEST_OPS_PIECE_MIB", 256), 1) * mib;
    const auto files = static_cast<std::size_t>(number_from_env("GRAB_TEST_OPS_FILES", 2000));
    const auto width = static_cast<std::size_t>(std::max<std::uint64_t>(number_from_env("GRAB_TEST_OPS_IN_FLIGHT", 32), 1));
    std::vector<std::uint64_t> sizes;
    for (const auto& s : util::split(util::getenv_utf8("GRAB_TEST_OPS_MIB").value_or("256"), ','))
        if (!s.empty()) sizes.push_back(std::stoull(s) * mib);

#ifdef _WIN32
    const test::AppData settings(util::path_from_utf8(*appdata));
#endif
    auto ctx = test::read_context(*name);
    REQUIRE_MESSAGE(ctx, ctx.error_or(""));
    auto target = test::live_target(*ctx);
    REQUIRE_MESSAGE(target, target.error_or(""));
    say(std::format("server ops on {} ({}); folder {}", *name, *modes, d.dir.empty() ? "none" : d.dir));

    std::atomic<int> logins = 0;
    ssh::Pool::Options o;
    o.limit = 4;
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
        const bool in_dir_writes = wanted("links") || wanted("copy") || wanted("tree");
        if (!d.dir.empty() && in_dir_writes && !ensure_dir(s, d)) return;
        if (wanted("caps")) caps(s, d, t);
        if (wanted("names")) names(s, d);
        if (wanted("links")) links(s, d);
    }
    if (wanted("copy")) {
        const Data data(0xC0DA7A);
        bool offered = false;
        {
            auto lease = pool->acquire({}, ssh::interactive_retry);
            REQUIRE(lease);
            offered = (*lease)->sftp().has_extension(sftp::copy_data_extension);
        }
        if (!offered) {
            say("copy: copy-data not offered: not tried");
        } else if (const auto source = source_file(*pool, d, source_size, data)) {
            for (const std::uint64_t size : sizes) copy_one(*pool, d, *source, source_size, data, size, t.timeout, piece);
        }
    }
    if (wanted("tree")) tree(*pool, d, files, width);
    if (wanted("remove") || wanted("readonly") || wanted("cleanup")) {
        auto lease = pool->acquire({}, ssh::interactive_retry);
        REQUIRE(lease);
        auto& s = (*lease)->sftp();
        if (wanted("remove")) remove_trees(s, d, 2 * width);
        if (wanted("readonly")) {
            for (const std::string& dir : ro_dirs) CHECK(readonly(s, dir));
        }
        if (wanted("cleanup")) cleanup(s, d, 2 * width);
    }
    say(std::format("{} login(s) in all; {:.1f} s", logins.load(), seconds(Clock::now() - started)));
}

TEST_CASE("hook helpers: the server operations' folder rule and the tree's layout") {
    CHECK(ops_dir_allowed("/tmp/grab-ops-1"));
    CHECK(ops_dir_allowed("/tmp/grab-ops-boxtest-ab12"));
    CHECK(ops_dir_allowed("grab-ops-spike-12ab"));
    CHECK_FALSE(ops_dir_allowed("/tmp/grab-ops-"));
    CHECK_FALSE(ops_dir_allowed("/tmp/grab-ops-x/sub"));
    CHECK_FALSE(ops_dir_allowed("/tmp/grab-up-x"));
    CHECK_FALSE(ops_dir_allowed("/tmp/other"));
    CHECK_FALSE(ops_dir_allowed("/tmp"));
    CHECK_FALSE(ops_dir_allowed("/home/dragon/grab-ops-x"));
    CHECK_FALSE(ops_dir_allowed("grab-ops-spike-"));
    CHECK_FALSE(ops_dir_allowed("grab-ops-spike-1/sub"));
    CHECK_FALSE(ops_dir_allowed("grab-ops-x"));
    CHECK_FALSE(ops_dir_allowed("/grab-ops-spike-1"));
    CHECK_FALSE(ops_dir_allowed(""));
    CHECK_FALSE(ops_dir_allowed("/tmp//grab-ops-x"));

    const auto folders = tree_folders();
    CHECK(folders.size() == 24);
    CHECK(folders.front() == "d00");
    CHECK(folders.back() == "deep/a/b/c");
    const auto files = tree_files(2000);
    CHECK(files.size() == 2000);
    CHECK(files[0].rel == "d00/f0000.bin");
    CHECK(files[0].size == 0);
    CHECK(files[21].rel == "d01/f0021.bin");
    CHECK(files[21].size == (21 * 7919) % 16384 + 1);
    std::uint64_t most = 0;
    for (const TreeFile& f : files) most = std::max(most, f.size);
    CHECK(most <= 16384);
    CHECK(depth_of("deep/a/b") == 2);
    CHECK(under("/tmp/x", "") == "/tmp/x");
    CHECK(under("/tmp/x", "d00/f") == "/tmp/x/d00/f");

    const Data data(3);
    const std::string one = tree_bytes(data, 5, 100);
    CHECK(one.size() == 100);
    CHECK(one == tree_bytes(data, 5, 100));
    CHECK(one != tree_bytes(data, 6, 100));
    std::string twice(std::size_t{2000}, '\0');
    data.fill(0, std::span(twice.data(), 1000));
    data.fill(0, std::span(twice.data() + 1000, 1000));
    CHECK(repeated_md5(data, 1000, 2) == md5_of(twice));
    CHECK(repeated_md5(data, 1000, 1) == data.md5(1000));
}
