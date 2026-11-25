#include <doctest/doctest.h>

// The server file operations' engine (server_files.hpp; SERVER-OPS.md, chunk 1) on a live server,
// printed only: grab-plans\server-ops\live1-main.ps1 runs it on Main (the live checker). The
// server is loaded from the settings the programs use, read only (live_settings.hpp). Everything
// it makes goes into GRAB_TEST_FILES_DIR, which must be /tmp/grab-ops-<x> in plain characters
// (made here when it isn't there); 'readonly' only tries changes in GRAB_TEST_FILES_RO_DIR (a
// chmod 555 /tmp/grab-ops-* folder the script made, holding keep.txt) and proves nothing there
// changed. Only codes, counts and yes/no are printed (no name but the folders given; an owner's
// name only as its length). Lines start with "hook: ".
//
//   GRAB_TEST_SERVER        the server's name in grab.conf
//   GRAB_TEST_APPDATA       the %APPDATA% the programs use (a copy of the settings); grab.conf there
//   GRAB_TEST_FILES         some of setup,rename,move,choices,big,tree,cancel,readonly,props,size,hash,delete,cleanup
//                           (comma-separated), run in that order
//   GRAB_TEST_FILES_DIR     the folder (/tmp/grab-ops-<x>)
//   GRAB_TEST_FILES_RO_DIR  readonly: the 555 folder
//   GRAB_TEST_FILES_MIB     big: the file's size in MiB (1024), built on the server from copies of a
//                           64 MiB file written over 4 connections
//   GRAB_TEST_FILES_COUNT   tree: its files (2000), with hidden ones and links among them
//
// At most 4 logins at once.

#include "config.hpp"
#include "fetch.hpp"
#include "live_settings.hpp"
#include "live_tools.hpp"
#include "server_files.hpp"
#include "sftp.hpp"
#include "ssh.hpp"
#include "ssh_hash.hpp"
#include "ssh_pool.hpp"
#include "upload_existing.hpp"
#include "util.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <format>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace grab;
using namespace grab::test::live;
using upload::Existing;

namespace {

// /tmp/grab-ops-<x>, <x> in plain characters (letters, digits, '.', '-', '_'), nothing below it.
bool files_dir_allowed(std::string_view dir) {
    constexpr std::string_view prefix = "/tmp/grab-ops-";
    return dir.starts_with(prefix) && dir.size() > prefix.size() && std::ranges::all_of(dir.substr(prefix.size()), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '.' || c == '-' || c == '_';
           });
}

struct Live {
    ssh::Pool& pool;
    server_files::Where w;
    RcloneRemote remote;
    std::string dir; // plain: grab's form and the raw bytes are the same
    [[nodiscard]] std::string at(std::string_view rel) const { return std::format("{}/{}", dir, rel); }
};

// `step` on one connection of the pool.
template <class F> auto on_lease(ssh::Pool& pool, F&& step) {
    auto lease = pool.acquire({}, ssh::interactive_retry);
    REQUIRE(lease);
    return step((*lease)->sftp());
}

bool exists(ssh::Pool& pool, const std::string& path) {
    return on_lease(pool, [&](sftp::Client& c) { return sftp::lstat(c, path).has_value(); });
}

std::string read_text(ssh::Pool& pool, const std::string& path) {
    return on_lease(pool, [&](sftp::Client& c) {
        auto r = read_small(c, path);
        return r ? *r : std::string("<") + code_of(r) + ">";
    });
}

// A new file of `bytes` with the time `mtime` (whole seconds, so copies compare equal) and `mode`.
void put(ssh::Pool& pool, const std::string& path, std::string_view bytes, std::uint32_t mtime = 1'700'000'000, std::uint32_t mode = 0644) {
    on_lease(pool, [&](sftp::Client& c) {
        REQUIRE(create_file(c, path, bytes));
        REQUIRE(sftp::setstat(c, path, sftp::Attrs{.size = {}, .permissions = mode, .mtime = mtime, .uid = {}, .gid = {}, .atime = {}}));
        return true;
    });
}

void mkdirs(ssh::Pool& pool, std::initializer_list<std::string> paths) {
    on_lease(pool, [&](sftp::Client& c) {
        for (const std::string& p : paths) REQUIRE(sftp::mkdir(c, p));
        return true;
    });
}

// A command on the server (plain paths in single quotes): its exit status and stdout (64 KiB at most).
struct Shell {
    int status = -1;
    std::string out;
};
Shell shell(ssh::Pool& pool, const std::string& command) {
    auto lease = pool.acquire({}, ssh::interactive_retry);
    REQUIRE(lease);
    Shell s;
    ssh::ExecOptions o;
    o.out = [&](std::string_view piece) {
        s.out.append(piece.substr(0, 65536 - std::min<std::size_t>(65536, s.out.size())));
        return true;
    };
    o.dead_after = std::chrono::hours(1);
    o.deadline = std::chrono::minutes(10);
    auto r = (*lease)->exec(command, o);
    if (r) s.status = r->status;
    return s;
}

// What a tree holds, as one line of sums: its files (path, size, mode, time; their bytes' MD5s),
// its links (where each points) and its folders (path, mode). Equal for a copy, or not.
std::string tree_sums(ssh::Pool& pool, const std::string& dir) {
    const Shell s = shell(pool, std::format("cd '{}' && {{ find . -type f -printf '%p %s %m %T@\\n' | LC_ALL=C sort | md5sum; "
                                            "find . -type f -print0 | LC_ALL=C sort -z | xargs -0 md5sum | md5sum; "
                                            "find . -type l -printf '%p -> %l\\n' | LC_ALL=C sort | md5sum; "
                                            "find . -mindepth 1 -type d -printf '%p %m\\n' | LC_ALL=C sort | md5sum; }}",
                                            dir));
    return s.status == 0 ? s.out : std::format("<exit {}>", s.status);
}

std::size_t count_of(ssh::Pool& pool, const std::string& command) {
    const Shell s = shell(pool, command);
    try {
        return s.status == 0 ? static_cast<std::size_t>(std::stoull(s.out)) : 999'999;
    } catch (const std::exception&) {
        return 999'999;
    }
}

std::size_t temps_below(ssh::Pool& pool, const std::string& dir) {
    return count_of(pool, std::format("find '{}' -name '*.grabpart' | wc -l", dir));
}

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

std::string outcome(const server_files::Report& r) {
    std::string out = std::format("{} file(s), {} folder(s)", r.files, r.folders);
    std::size_t skipped = 0, failed = 0, asking = 0;
    for (const auto& e : r.entries) {
        (e.state == server_files::Entry::State::skipped ? skipped : e.state == server_files::Entry::State::failed ? failed : asking) += 1;
    }
    if (skipped + failed + asking > 0) out += std::format(", {} skipped, {} failed, {} asking", skipped, failed, asking);
    if (r.stopped) out += ", stopped";
    if (!r.error.empty()) out += std::format(", error \"{}\"", r.error);
    return out;
}

std::vector<server_files::Item> one(const std::string& from, const std::string& into) {
    const std::vector<std::string> p{from};
    return server_files::into(p, into);
}

// ---- the modes ------------------------------------------------------------------------------------------

void rename_mode(const Live& l) {
    mkdirs(l.pool, {l.at("rn"), l.at("rn/d")});
    put(l.pool, l.at("rn/a.txt"), "alpha");
    put(l.pool, l.at("rn/b.txt"), "bravo");
    put(l.pool, l.at("rn/d/in.txt"), "inside");
    auto file = server_files::rename(l.w, l.at("rn/a.txt"), "c.txt");
    auto folder = server_files::rename(l.w, l.at("rn/d"), "e");
    auto taken = server_files::rename(l.w, l.at("rn/c.txt"), "b.txt");
    const bool file_ok = file && !file->taken && read_text(l.pool, l.at("rn/c.txt")) == "alpha" && !exists(l.pool, l.at("rn/a.txt"));
    const bool folder_ok = folder && !folder->taken && read_text(l.pool, l.at("rn/e/in.txt")) == "inside" && !exists(l.pool, l.at("rn/d"));
    const bool taken_ok = taken && taken->taken && read_text(l.pool, l.at("rn/b.txt")) == "bravo" &&
                          read_text(l.pool, l.at("rn/c.txt")) == "alpha";
    say(std::format("rename: a file {}; a folder {}; onto a name taken: 'already taken', both kept {}", pass(file_ok), pass(folder_ok),
                    pass(taken_ok)));
}

void move_mode(const Live& l) {
    mkdirs(l.pool, {l.at("mv"), l.at("mv/d"), l.at("mv/into")});
    put(l.pool, l.at("mv/a.txt"), "alpha");
    put(l.pool, l.at("mv/d/in.txt"), "inside");
    put(l.pool, l.at("mv/target.txt"), "the link's target");
    on_lease(l.pool, [&](sftp::Client& c) { return sftp::symlink(c, l.at("mv/link"), "target.txt").has_value(); });
    const std::vector<std::string> picked{l.at("mv/a.txt"), l.at("mv/d"), l.at("mv/link")};
    const auto r = server_files::move(l.w, server_files::into(picked, l.at("mv/into")), Existing::ask);
    const bool moved = r.error.empty() && r.files == 2 && r.folders == 1 && read_text(l.pool, l.at("mv/into/a.txt")) == "alpha" &&
                       read_text(l.pool, l.at("mv/into/d/in.txt")) == "inside" && !exists(l.pool, l.at("mv/a.txt")) &&
                       !exists(l.pool, l.at("mv/d"));
    const bool link = on_lease(l.pool, [&](sftp::Client& c) {
        auto own = sftp::lstat(c, l.at("mv/into/link"));
        auto to = sftp::readlink(c, l.at("mv/into/link"));
        return own && own->is_link() && to && *to == "target.txt";
    }) && read_text(l.pool, l.at("mv/target.txt")) == "the link's target";
    say(std::format("move: a file, a folder and a link into a subfolder: {} ({}); the link moved as itself, its target untouched {}",
                    pass(moved), outcome(r), pass(link)));
    const std::string before = tree_sums(l.pool, l.at("mv"));
    const auto self = server_files::move(l.w, one(l.at("mv/into/d"), l.at("mv/into/d")), Existing::replace);
    const auto below = server_files::move(l.w, one(l.at("mv/into"), l.at("mv/into/d")), Existing::replace);
    const bool refused = self.error == "d can't be moved into itself" && below.error == "into can't be moved into itself" &&
                         tree_sums(l.pool, l.at("mv")) == before;
    say(std::format("move: into itself and into its own subfolder refused before anything was sent, nothing changed {}", pass(refused)));
}

// Each existing-file choice, on a move and on a copy: the file there and the one coming, by their
// bytes and times; what is at the name afterwards.
void choices_mode(const Live& l) {
    struct Case {
        const char* name;
        Existing choice;
        const char* there;
        std::uint32_t there_time;
        enum { replaced, kept, numbered, asked } expect;
    };
    constexpr std::uint32_t coming_time = 1'700'500'000;
    const std::array<Case, 8> cases{{
        {"replace", Existing::replace, "old", 1'700'000'000, Case::replaced},
        {"different-same", Existing::different, "coming", coming_time, Case::kept},
        {"different-other", Existing::different, "other!", 1'700'000'000, Case::replaced},
        {"newer-newer", Existing::newer, "old", 1'700'000'000, Case::replaced},
        {"newer-older", Existing::newer, "new", coming_time + 100, Case::kept},
        {"skip", Existing::skip, "old", 1'700'000'000, Case::kept},
        {"keep-both", Existing::keep_both, "old", 1'700'000'000, Case::numbered},
        {"ask", Existing::ask, "old", 1'700'000'000, Case::asked},
    }};
    mkdirs(l.pool, {l.at("ch")});
    for (const bool copying : {false, true}) {
        std::string line = std::format("choices on a {}:", copying ? "copy" : "move");
        bool all = true;
        for (const Case& c : cases) {
            const std::string src = l.at(std::format("ch/{}-{}-src", copying ? "cp" : "mv", c.name));
            const std::string dst = l.at(std::format("ch/{}-{}-dst", copying ? "cp" : "mv", c.name));
            mkdirs(l.pool, {src, dst});
            put(l.pool, src + "/f.txt", "coming", coming_time);
            put(l.pool, dst + "/f.txt", c.there, c.there_time);
            const auto items = one(src + "/f.txt", dst);
            const auto r = copying ? server_files::copy(l.w, items, c.choice) : server_files::move(l.w, items, c.choice);
            const std::string at_name = read_text(l.pool, dst + "/f.txt");
            const bool source_kept = exists(l.pool, src + "/f.txt");
            bool ok = r.error.empty() && temps_below(l.pool, dst) == 0;
            switch (c.expect) {
            case Case::replaced: ok = ok && at_name == "coming" && source_kept == copying; break;
            case Case::kept: ok = ok && at_name == c.there && source_kept && !r.entries.empty(); break;
            case Case::numbered:
                ok = ok && at_name == c.there && read_text(l.pool, dst + "/f (2).txt") == "coming" && source_kept == copying;
                break;
            case Case::asked:
                ok = ok && at_name == c.there && source_kept && r.entries.size() == 1 &&
                     r.entries[0].state == server_files::Entry::State::asking && r.entries[0].clash &&
                     r.entries[0].clash->remote_size == std::string_view(c.there).size();
                break;
            }
            all = all && ok;
            line += std::format(" {} {};", c.name, pass(ok));
        }
        say(line + std::format(" all: {}", pass(all)));
    }
}

void big_mode(const Live& l, std::uint64_t size) {
    const std::uint64_t source_size = 64 * mib;
    const Data data(0xB16F11E);
    if (size == 0 || size % source_size != 0) {
        say(std::format("big: {} MiB isn't a multiple of 64 MiB: not tried", size / mib));
        return;
    }
    const std::string src = l.at("big-src.bin");
    const std::string big = l.at("big.bin");
    mkdirs(l.pool, {l.at("copies"), l.at("cancel")});
    on_lease(l.pool, [&](sftp::Client& c) { return create_file(c, src, "").has_value(); });
    const auto [parts, took] = write_parallel(l.pool, src, source_size, 4, data, 64, sftp::max_unacked, sftp::max_write);
    bool written = true;
    for (const Part& p : parts) written = written && p.failed.empty();
    // The big one built on the server from copies of the source at its offsets.
    const bool built = written && on_lease(l.pool, [&](sftp::Client& c) {
        auto from = sftp::open_file(c, src);
        auto to = sftp::open_file(c, big, sftp::OpenFlags::write | sftp::OpenFlags::creat | sftp::OpenFlags::excl);
        bool ok = from && to;
        for (std::uint64_t k = 0; ok && k < size / source_size; ++k)
            ok = sftp::copy_range(c, *from, 0, source_size, *to, k * source_size, std::chrono::seconds(30)).has_value();
        if (from) ok = sftp::close_file(c, *from).has_value() && ok;
        if (to) ok = sftp::close_file(c, *to).has_value() && ok;
        const sftp::Attrs kept{.size = {}, .permissions = 0640, .mtime = 1'700'123'456, .uid = {}, .gid = {}, .atime = {}};
        ok = ok && sftp::setstat(c, big, kept);
        return ok;
    });
    say(std::format("big: the source of 64 MiB written over 4 connections in {:.1f} s; {} MiB built from it on the server {}", took,
                    size / mib, pass(built)));
    if (!built) return;
    const std::string want = repeated_md5(data, source_size, size / source_size);

    std::size_t updates = 0;
    const auto t0 = Clock::now();
    const auto r = server_files::copy(l.w, one(big, l.at("copies")), Existing::ask, [&](const server_files::Progress&) { ++updates; });
    const double secs = seconds(Clock::now() - t0);
    const std::string copy = l.at("copies/big.bin");
    const auto md5 = server_md5(l.pool, copy);
    const bool attrs = on_lease(l.pool, [&](sftp::Client& c) {
        auto a = sftp::lstat(c, copy);
        return a && a->size == size && a->mtime == 1'700'123'456u && a->permissions && (*a->permissions & 0777) == 0640;
    });
    say(std::format("big: a copy of {} MiB on the server in {:.1f} s = {:.0f} MiB/s ({} progress updates): {}; md5sum equal {}; size, "
                    "time and mode kept {}; no temp left {}",
                    size / mib, secs, secs > 0 ? static_cast<double>(size) / mib / secs : 0.0, updates, outcome(r),
                    pass(md5 && *md5 == want), pass(attrs), pass(temps_below(l.pool, l.at("copies")) == 0)));

    // Stopped after its first piece: no temp, nothing at the name.
    std::stop_source stop;
    const auto s = server_files::copy(l.w, one(big, l.at("cancel")), Existing::ask,
                                      [&](const server_files::Progress& p) {
                                          if (p.bytes > 0) stop.request_stop();
                                      },
                                      stop.get_token());
    say(std::format("cancel: a big copy stopped after its first piece: stopped {}; nothing at its name {}; no temp left {}", pass(s.stopped),
                    pass(!exists(l.pool, l.at("cancel/big.bin"))), pass(temps_below(l.pool, l.at("cancel")) == 0)));
}

// The tree: folders d00..d19 holding the files, an empty chain deep/a/b/c, hidden files, modes and
// times of their own, a relative link in each folder, a link to a file outside the tree and one to
// a folder outside it (never walked into).
void make_tree(const Live& l, const std::string& root, std::size_t n) {
    const Data data(0x7EE);
    mkdirs(l.pool, {root});
    std::vector<std::string> folders;
    for (int i = 0; i < 20; ++i) folders.push_back(std::format("{}/d{:02}", root, i));
    for (const char* chain : {"deep", "deep/a", "deep/a/b", "deep/a/b/c"}) folders.push_back(std::format("{}/{}", root, chain));
    put(l.pool, l.at("outside.txt"), "outside the tree");
    on_lease(l.pool, [&](sftp::Client& c) {
        for (const std::string& f : folders) REQUIRE(sftp::mkdir(c, f));
        // Files, 32 at a time: OPEN, WRITE, FSETSTAT (a mode and a whole-second time), CLOSE.
        std::size_t next = 0, going = 0, failed = 0;
        while (next < n || going > 0) {
            while (going < 32 && next < n) {
                const std::size_t i = next++;
                ++going;
                const std::string name = i % 100 == 7 ? std::format(".hidden-{:04}", i) : std::format("f{:04}.bin", i);
                const std::string path = std::format("{}/d{:02}/{}", root, i % 20, name);
                const std::uint64_t size = i % 500 == 0 ? 0 : (i * 7919) % 16384 + 1;
                std::string bytes(static_cast<std::size_t>(size), '\0');
                data.fill(std::uint64_t{i} * 16384, std::span(bytes.data(), bytes.size()));
                const std::uint32_t mode = i % 7 == 0 ? 0600 : i % 11 == 0 ? 0755 : 0644;
                c.open(path, sftp::OpenFlags::write | sftp::OpenFlags::creat | sftp::OpenFlags::excl, {},
                       [&c, &going, &failed, bytes = std::move(bytes), mode, i](std::expected<std::string, sftp::Status> h) {
                           if (!h) {
                               ++failed;
                               --going;
                               return;
                           }
                           auto left = std::make_shared<int>(bytes.empty() ? 2 : 3);
                           const auto answered = [&going, &failed, left](std::expected<void, sftp::Status> r) {
                               if (!r) ++failed;
                               if (--*left == 0) --going;
                           };
                           if (!bytes.empty()) c.write(*h, 0, bytes, answered);
                           c.fsetstat(*h,
                                      sftp::Attrs{.size = {},
                                                  .permissions = mode,
                                                  .mtime = static_cast<std::uint32_t>(1'700'000'000 + i),
                                                  .uid = {},
                                                  .gid = {},
                                                  .atime = {}},
                                      answered);
                           c.close(*h, answered);
                       });
            }
            if (going > 0) c.pump();
        }
        REQUIRE(failed == 0);
        for (int i = 0; i < 20; ++i)
            REQUIRE(sftp::symlink(c, std::format("{}/d{:02}/ln", root, i), std::format("f{:04}.bin", i + 20)));
        REQUIRE(sftp::symlink(c, root + "/out-link", l.at("outside.txt")));
        REQUIRE(sftp::symlink(c, root + "/dir-link", l.dir));
        return true;
    });
}

void tree_mode(const Live& l, std::size_t n) {
    const std::string root = l.at("tree");
    const auto t0 = Clock::now();
    make_tree(l, root, n);
    say(std::format("tree: {} files (hidden ones among them), 24 folders and 22 links made in {:.1f} s", n, seconds(Clock::now() - t0)));
    mkdirs(l.pool, {l.at("tree-copies")});
    const auto t1 = Clock::now();
    const auto r = server_files::copy(l.w, one(root, l.at("tree-copies")), Existing::ask);
    const double secs = seconds(Clock::now() - t1);
    const std::string source = tree_sums(l.pool, root);
    const std::string copied = tree_sums(l.pool, l.at("tree-copies/tree"));
    const bool links = on_lease(l.pool, [&](sftp::Client& c) {
        auto out = sftp::readlink(c, l.at("tree-copies/tree/out-link"));
        auto dir = sftp::readlink(c, l.at("tree-copies/tree/dir-link"));
        auto ln = sftp::lstat(c, l.at("tree-copies/tree/d03/ln"));
        return out && *out == l.at("outside.txt") && dir && *dir == l.dir && ln && ln->is_link();
    });
    say(std::format("tree copy: {} in {:.1f} s = {:.0f} files/s; files (bytes, sizes, modes, times), links and folders equal {}; links "
                    "made again as links {}; no temp left {}",
                    outcome(r), secs, secs > 0 ? static_cast<double>(r.files) / secs : 0.0,
                    pass(source.size() > 20 && source == copied), pass(links), pass(temps_below(l.pool, l.at("tree-copies")) == 0)));
}

void cancel_mode(const Live& l) {
    const std::string root = l.at("tree");
    mkdirs(l.pool, {l.at("cancel-tree")});
    std::stop_source stop;
    const auto r = server_files::copy(l.w, one(root, l.at("cancel-tree")), Existing::ask,
                                      [&](const server_files::Progress& p) {
                                          if (p.files >= 300) stop.request_stop();
                                      },
                                      stop.get_token());
    const std::string copy = l.at("cancel-tree/tree");
    const std::size_t empty = count_of(l.pool, std::format("find '{}' -mindepth 1 -type d -empty | wc -l", l.at("cancel-tree")));
    const Shell same = shell(l.pool, std::format("cd '{}' 2>/dev/null || exit 0; find . -type f -print0 | xargs -0 -r md5sum | "
                                                 "(cd '{}' && md5sum -c --quiet -)",
                                                 copy, root));
    say(std::format("cancel: a tree copy stopped after 300 files: {}; no temp left {}; no empty folder of its own left {} ({}); what "
                    "was copied equal to its source {}",
                    outcome(r), pass(temps_below(l.pool, l.at("cancel-tree")) == 0), pass(empty == 0), empty,
                    pass(same.status == 0 && same.out.empty())));
}

void readonly_mode(const Live& l, const std::string& ro) {
    const std::string before = tree_sums(l.pool, ro);
    put(l.pool, l.at("ro-mover.txt"), "mover");
    const auto renamed = server_files::rename(l.w, ro + "/keep.txt", "other.txt");
    const auto made = server_files::mkdir(l.w, ro, "new");
    const std::vector<std::string> keep{ro + "/keep.txt"};
    const auto removed = server_files::remove(l.w, keep);
    const auto copied = server_files::copy(l.w, one(l.at("ro-mover.txt"), ro), Existing::ask);
    const auto moved = server_files::move(l.w, one(l.at("ro-mover.txt"), ro), Existing::ask);
    const std::string said = std::format("{} doesn't let this account write in {}", l.w.server, ro);
    const std::string move_said = std::format("{} doesn't let this account move ro-mover.txt from {} into {}", l.w.server, l.dir, ro);
    const std::string moved_why = moved.entries.empty() ? moved.error : moved.entries[0].why;
    // A folder that refuses new files fails only the files for it: the copy's entry says so.
    const std::string copied_why = copied.entries.empty() ? copied.error : copied.entries[0].why;
    const bool words = !renamed && renamed.error() == said && !made && made.error() == said &&
                       removed.error == said + "; nothing was deleted" && copied.error.empty() && copied_why == said &&
                       moved.error.empty() && moved_why == move_said;
    const bool unchanged = tree_sums(l.pool, ro) == before && read_text(l.pool, l.at("ro-mover.txt")) == "mover";
    say(std::format("readonly {}: rename, new folder, delete, copy and move into it refused in plain words {}; nothing changed {}", ro,
                    pass(words), pass(unchanged)));
    if (!words) {
        say(std::format("readonly: the words were: \"{}\" / \"{}\" / \"{}\" / \"{}\" / \"{}\"", renamed ? "" : renamed.error(),
                        made ? "" : made.error(), removed.error, copied_why, moved_why));
    }
}

void props_mode(const Live& l) {
    put(l.pool, l.at("props.txt"), "properties", 1'700'000'123, 0640);
    on_lease(l.pool, [&](sftp::Client& c) { return sftp::symlink(c, l.at("props-link"), "props.txt").has_value(); });
    const auto file = server_files::properties(l.w, l.at("props.txt"));
    const auto folder = server_files::properties(l.w, l.dir);
    const auto link = server_files::properties(l.w, l.at("props-link"));
    const bool file_ok = file && file->kind == server_files::Properties::Kind::file && file->attrs.size == 10u &&
                         file->attrs.mtime == 1'700'000'123u && file->attrs.permissions && (*file->attrs.permissions & 0777) == 0640;
    const bool link_ok = link && link->kind == server_files::Properties::Kind::link && link->link == "props.txt" && link->target &&
                         link->target->size == 10u;
    say(std::format("props: a file's size, time and mode {}; a folder {}; a link's target and what it leads to {}; owner and group named "
                    "{} ({} and {} characters); free space {}; read-only {}",
                    pass(file_ok), pass(folder && folder->kind == server_files::Properties::Kind::folder), pass(link_ok),
                    yes(file && file->names), file ? file->owner.size() : 0, file ? file->group.size() : 0,
                    file && file->free ? util::format_size(*file->free) : std::string("none"), file && file->read_only ? "yes" : "no"));
}

void size_mode(const Live& l, std::size_t n) {
    std::uint64_t bytes = 0;
    for (std::size_t i = 0; i < n; ++i) bytes += i % 500 == 0 ? 0 : (i * 7919) % 16384 + 1;
    const std::vector<std::string> tree{l.at("tree")};
    std::size_t updates = 0;
    const auto s = server_files::folder_size(l.w, tree, [&](const server_files::Progress&) { ++updates; });
    const bool ok = s && s->files == n && s->folders == 25 && s->links == 22 && s->bytes == bytes && s->unreadable.empty();
    say(std::format("size: the tree's files, folders, links and bytes {} ({} progress updates)", pass(ok), updates));
}

void hash_mode(const Live& l) {
    const std::string big = l.at("big.bin");
    const std::string copy = l.at("copies/big.bin");
    const auto size = on_lease(l.pool, [&](sftp::Client& c) {
        auto a = sftp::lstat(c, big);
        return a && a->size ? *a->size : 0;
    });
    if (size == 0) {
        say("hash: no big file ('big' first): not tried");
        return;
    }
    const auto t0 = Clock::now();
    const auto md5_a = server_files::hash(l.w, l.remote, big, size, ssh::Sum::md5);
    const auto md5_b = server_files::hash(l.w, l.remote, copy, size, ssh::Sum::md5);
    const auto sha_a = server_files::hash(l.w, l.remote, big, size, ssh::Sum::sha256);
    const auto sha_b = server_files::hash(l.w, l.remote, copy, size, ssh::Sum::sha256);
    const auto server = server_md5(l.pool, copy);
    say(std::format("hash: MD5 of the copy equal to the source's {} (and to md5sum's {}); SHA-256 equal {} ({} digits); 4 sums in {:.1f} s",
                    pass(md5_a && md5_b && *md5_a == *md5_b), pass(md5_b && server && *md5_b == *server),
                    pass(sha_a && sha_b && *sha_a == *sha_b), sha_a ? sha_a->size() : 0, seconds(Clock::now() - t0)));
}

void delete_mode(const Live& l) {
    const auto outside = server_md5(l.pool, l.at("outside.txt"));
    const std::string copy = l.at("tree-copies/tree");
    const std::size_t hidden = count_of(l.pool, std::format("find '{}' -name '.*' -type f | wc -l", copy));
    const auto t0 = Clock::now();
    const std::vector<std::string> paths{copy};
    std::size_t updates = 0;
    const auto r = server_files::remove(l.w, paths, [&](const server_files::Progress&) { ++updates; });
    const double secs = seconds(Clock::now() - t0);
    const auto after = server_md5(l.pool, l.at("outside.txt"));
    say(std::format("delete: the copied tree ({} hidden files in it, links to a file and a folder outside it): {} in {:.1f} s = {:.0f} "
                    "files/s ({} progress updates); gone {}; the links' targets outside still there with their md5 {}",
                    hidden, outcome(r), secs, secs > 0 ? static_cast<double>(r.files) / secs : 0.0, updates,
                    pass(!exists(l.pool, copy) && r.error.empty() && hidden > 0),
                    pass(outside && after && *outside == *after && exists(l.pool, l.dir) && exists(l.pool, l.at("tree/d00")))));
}

void cleanup_mode(const Live& l) {
    const std::vector<std::string> all{l.dir};
    const auto r = server_files::remove(l.w, all);
    say(std::format("cleanup: {}; the folder gone {}", outcome(r), pass(!exists(l.pool, l.dir))));
}

} // namespace

TEST_CASE("live server: the server file operations' engine, printed only (GRAB_TEST_FILES)") {
    const auto name = util::getenv_utf8("GRAB_TEST_SERVER");
    const auto appdata = util::getenv_utf8("GRAB_TEST_APPDATA");
    const auto modes = util::getenv_utf8("GRAB_TEST_FILES");
    if (!name || !appdata || !modes) {
        MESSAGE("skipped: GRAB_TEST_SERVER / GRAB_TEST_APPDATA / GRAB_TEST_FILES not set");
        return;
    }
    const auto list = util::split(*modes, ',');
    const auto wanted = [&](std::string_view mode) { return std::ranges::find(list, mode) != list.end(); };
    const std::string dir = util::getenv_utf8("GRAB_TEST_FILES_DIR").value_or("");
    const std::string why = "GRAB_TEST_FILES_DIR must be /tmp/grab-ops-<x> (got \"" + dir + "\")";
    REQUIRE_MESSAGE(files_dir_allowed(dir), why);
    const std::string ro = util::getenv_utf8("GRAB_TEST_FILES_RO_DIR").value_or("");
    REQUIRE_MESSAGE((!wanted("readonly") || files_dir_allowed(ro)), "'readonly' needs GRAB_TEST_FILES_RO_DIR (/tmp/grab-ops-<x>)");
    const std::uint64_t big = number_from_env("GRAB_TEST_FILES_MIB", 1024) * mib;
    const auto count = static_cast<std::size_t>(number_from_env("GRAB_TEST_FILES_COUNT", 2000));

#ifdef _WIN32
    const test::AppData settings(util::path_from_utf8(*appdata));
#endif
    auto ctx = test::read_context(*name);
    REQUIRE_MESSAGE(ctx, ctx.error_or(""));
    auto target = test::live_target(*ctx);
    REQUIRE_MESSAGE(target, target.error_or(""));
    say(std::format("server files on {} ({}); folder {}", *name, *modes, dir));

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
    fetch::Connections connections;
    connections.configure(*name, 4);
    const Live l{.pool = *pool,
                 .w = server_files::Where{.server = *name,
                                          .pool = pool,
                                          .connections = &connections,
                                          .timeout = t.timeout,
                                          .on_command = {},
                                          .home = {},
                                          .on_undo = {}},
                 .remote = ctx->remote,
                 .dir = dir};
    const auto started = Clock::now();
    if (wanted("setup")) {
        const bool there = exists(*pool, dir);
        if (!there) mkdirs(*pool, {dir});
        say(std::format("setup: folder {}: {}", dir, there ? "there already" : "made"));
    }
    if (wanted("rename")) rename_mode(l);
    if (wanted("move")) move_mode(l);
    if (wanted("choices")) choices_mode(l);
    if (wanted("big")) big_mode(l, big);
    if (wanted("tree")) tree_mode(l, count);
    if (wanted("cancel")) cancel_mode(l);
    if (wanted("readonly")) readonly_mode(l, ro);
    if (wanted("props")) props_mode(l);
    if (wanted("size")) size_mode(l, count);
    if (wanted("hash")) hash_mode(l);
    if (wanted("delete")) delete_mode(l);
    if (wanted("cleanup")) cleanup_mode(l);
    say(std::format("{} login(s) in all; {:.1f} s", logins.load(), seconds(Clock::now() - started)));
}

TEST_CASE("hook helpers: the server file operations' folder rule") {
    CHECK(files_dir_allowed("/tmp/grab-ops-files-1a2b3c"));
    CHECK(files_dir_allowed("/tmp/grab-ops-ro-1"));
    CHECK_FALSE(files_dir_allowed("/tmp/grab-ops-"));
    CHECK_FALSE(files_dir_allowed("/tmp/grab-ops-x/sub"));
    CHECK_FALSE(files_dir_allowed("/tmp/grab-up-x"));
    CHECK_FALSE(files_dir_allowed("grab-ops-spike-1"));
    CHECK_FALSE(files_dir_allowed("/home/dragon/grab-ops-x"));
    CHECK_FALSE(files_dir_allowed("/tmp/grab-ops-x y"));
    CHECK_FALSE(files_dir_allowed(""));
}
