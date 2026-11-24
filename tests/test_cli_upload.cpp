#include <doctest/doctest.h>

// `grab upload` (cli_upload.hpp; UPLOADS.md 5.11): LOCAL's names, the answers typed at its
// question, and whole runs on an in-memory server every login writes (fake_pool.hpp), what each
// run says read back from a file in place of the console.

#include "cli.hpp"
#include "cli_console.hpp"
#include "cli_upload.hpp"
#include "engine.hpp"
#include "fake_md5sum.hpp"
#include "fake_pool.hpp"
#include "fake_sftp.hpp"
#include "fetch.hpp"
#include "process.hpp"
#include "rate.hpp"
#include "upload.hpp"
#include "upload_existing.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using namespace grab;
namespace fs = std::filesystem;

namespace {

constexpr std::int64_t local_time = 1'700'001'000;
constexpr std::uint64_t MiB = std::uint64_t{1} << 20;

struct Dir {
    fs::path path;
    explicit Dir(std::string_view name) : path(fs::temp_directory_path() / "grab-tests-cli-upload" / name) {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~Dir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    Dir(const Dir&) = delete;
    Dir& operator=(const Dir&) = delete;
    [[nodiscard]] fs::path records() const { return path / "records"; }
};

fs::path put_local(const fs::path& root, std::string_view rel, std::string_view content, std::int64_t mtime = local_time) {
    const fs::path p = root / util::path_from_utf8(rel);
    fs::create_directories(p.parent_path());
    REQUIRE(util::write_file_atomic(p, content));
    fs::last_write_time(p, std::chrono::clock_cast<std::chrono::file_clock>(std::chrono::sys_seconds{std::chrono::seconds{mtime}}));
    return p;
}

std::string text(const fs::path& p) { return util::path_to_utf8(p); }

// The local files and folders below `root`, each file's bytes and time: what an upload never
// changes.
std::map<std::string, std::string> local_tree(const fs::path& root) {
    std::map<std::string, std::string> out;
    for (const auto& e : fs::recursive_directory_iterator(root)) {
        const std::string rel = text(fs::relative(e.path(), root));
        if (e.is_directory()) {
            out["dir " + rel] = "";
            continue;
        }
        const auto bytes = util::read_file(e.path());
        out[rel] = bytes.value_or("?") + "@" + std::to_string(fs::last_write_time(e.path()).time_since_epoch().count());
    }
    return out;
}

// A server whose logins all write one store, with the folder /up in it.
struct Server {
    test::FakePool fake;
    Server() {
        fake.writable = true;
        fake.statvfs = true;
        fake.quota_bytes = 256 * MiB;
        fake.store.dirs["/"] = {".", ".."};
        folder("/up");
    }
    void folder(const std::string& path) {
        std::scoped_lock lock(fake.store.mutex);
        fake.store.dirs[path] = {".", ".."};
        const auto slash = path.rfind('/');
        fake.store.dirs[slash == 0 ? "/" : path.substr(0, slash)].push_back(path.substr(slash + 1));
    }
    void put(const std::string& path, std::string content, std::int64_t mtime) {
        std::scoped_lock lock(fake.store.mutex);
        fake.store.files[path] = std::move(content);
        fake.store.mtimes[path] = static_cast<std::uint32_t>(mtime);
        const auto slash = path.rfind('/');
        fake.store.dirs[slash == 0 ? "/" : path.substr(0, slash)].push_back(path.substr(slash + 1));
    }
    [[nodiscard]] std::map<std::string, std::string> tree() {
        std::scoped_lock lock(fake.store.mutex);
        std::map<std::string, std::string> all = fake.store.files;
        for (const auto& [d, names] : fake.store.dirs) {
            std::vector<std::string> sorted = names;
            std::ranges::sort(sorted);
            all["dir " + d] = util::join(sorted, "/");
        }
        for (const auto& [f, t] : fake.store.mtimes) all["time " + f] = std::to_string(t);
        return all;
    }
    [[nodiscard]] std::optional<std::string> file(const std::string& path) {
        std::scoped_lock lock(fake.store.mutex);
        const auto it = fake.store.files.find(path);
        if (it == fake.store.files.end()) return std::nullopt;
        return it->second;
    }
    [[nodiscard]] std::uint32_t mtime(const std::string& path) {
        std::scoped_lock lock(fake.store.mutex);
        const auto it = fake.store.mtimes.find(path);
        return it == fake.store.mtimes.end() ? 0 : it->second;
    }
    // Temps of grab's in `dir`, and the bytes the biggest one holds.
    [[nodiscard]] std::pair<std::size_t, std::uint64_t> temps(const std::string& dir) {
        std::scoped_lock lock(fake.store.mutex);
        std::size_t n = 0;
        std::uint64_t most = 0;
        for (const auto& name : fake.store.dirs[dir]) {
            if (!upload::is_temp_name(name)) continue;
            ++n;
            if (const auto it = fake.store.files.find(dir + "/" + name); it != fake.store.files.end()) {
                most = std::max<std::uint64_t>(most, it->second.size());
            }
        }
        return {n, most};
    }
    // Bytes of every WRITE the closed sessions sent, and every request's name.
    [[nodiscard]] std::uint64_t written() {
        std::scoped_lock lock(fake.mutex);
        std::uint64_t n = 0;
        for (const auto& w : fake.writes) n += w.length;
        return n;
    }
    [[nodiscard]] std::vector<std::string> requests() {
        std::scoped_lock lock(fake.mutex);
        return fake.requests;
    }
    void forget() {
        std::scoped_lock lock(fake.mutex);
        fake.writes.clear();
        fake.requests.clear();
    }
};

// One `grab upload` run on the server: its console a file read back (said()), the answers typed
// taken from `answers` (none left: the input ended), its prompts kept.
struct Run {
    Server& srv;
    fs::path said_path;
    std::FILE* out = nullptr;
    std::optional<LiveLine> line;
    fetch::Connections connections;
    rate::RateLimiter limiter;
    UploadSetup s;
    std::vector<std::string> answers;
    std::vector<std::string> prompts;

    Run(Server& server, const Dir& dir, std::string_view name, int budget = 4) : srv(server), said_path(dir.path / util::path_from_utf8(name)) {
#ifdef _WIN32
        if (_wfopen_s(&out, said_path.c_str(), L"w+b") != 0) out = nullptr;
#else
        out = std::fopen(said_path.c_str(), "w+b");
#endif
        REQUIRE(out != nullptr);
        line.emplace(out);
        connections.configure("Fake", budget);
        s.server = "Fake";
        s.pool = srv.fake.pool(budget);
        s.connections = &connections;
        s.budget = budget;
        s.records_dir = dir.records();
        s.limiter = &limiter;
        s.line = &*line;
        s.ask = [this](const std::string& prompt) -> std::optional<std::string> {
            prompts.push_back(prompt);
            if (answers.empty()) return std::nullopt;
            std::string a = answers.front();
            answers.erase(answers.begin());
            return a;
        };
    }
    ~Run() {
        line.reset();
        if (out != nullptr) std::fclose(out);
    }
    Run(const Run&) = delete;
    Run& operator=(const Run&) = delete;

    int operator()(const std::vector<std::string>& argv, std::stop_token stop = {}) {
        const auto parsed = parse_args(argv);
        REQUIRE_MESSAGE(parsed.has_value(), parsed.error_or(""));
        const auto locals = expand_locals(parsed->opts.locals);
        REQUIRE_MESSAGE(locals.has_value(), locals.error_or(""));
        s.stop = std::move(stop);
        return run_upload(parsed->opts, *locals, s);
    }
    // Everything it said so far.
    std::string said() {
        std::fflush(out);
        std::fseek(out, 0, SEEK_SET);
        std::string all;
        std::array<char, 4096> buf{};
        for (std::size_t n = 0; (n = std::fread(buf.data(), 1, buf.size(), out)) > 0;) all.append(buf.data(), n);
        std::fseek(out, 0, SEEK_END);
        return all;
    }
    // Its sessions closed, so the server's counts are complete.
    void close() { s.pool.reset(); }
};

bool has(const std::string& all, std::string_view part) { return all.find(part) != std::string::npos; }

} // namespace

TEST_CASE("grab upload: * and ? in LOCAL pick names, ignoring case as Windows does") {
    CHECK(local_glob_match("*.jpg", "a.JPG"));
    CHECK(local_glob_match("A?C", "abc"));
    CHECK_FALSE(local_glob_match("a?c", "ac"));
    CHECK(local_glob_match("*", ".git"));
    CHECK(local_glob_match("a*b*c", "aXXbYYc"));
    CHECK_FALSE(local_glob_match("a*b*c", "aXXbYY"));
    CHECK(local_glob_match("*.t*t", "a.txt"));
    CHECK(local_glob_match("[a].txt", "[a].txt")); // no sets: brackets are a name's own
    CHECK_FALSE(local_glob_match("[a].txt", "a.txt"));
    CHECK(local_glob_match("\xC3\xBC*", "\xC3\x9C" "ber.txt"));   // ü* matches Über.txt
    CHECK(local_glob_match("?.txt", "\xE1\x83\x90.txt"));         // one Georgian letter, three bytes
    CHECK_FALSE(local_glob_match("??.txt", "\xE1\x83\x90.txt"));
    CHECK(local_glob_match("?", "\xF0\x9F\x98\x80"));             // outside the BMP: one too
    CHECK(local_glob_match("", ""));
    CHECK_FALSE(local_glob_match("", "a"));
}

TEST_CASE("grab upload: LOCAL expanded: a pattern's names sorted, a folder's last separator dropped, one not there refused") {
    Dir d("expand");
    put_local(d.path, "b.jpg", "b");
    put_local(d.path, "A.JPG", "a");
    put_local(d.path, "c.txt", "c");
    fs::create_directories(d.path / "sub");
    const std::string root = text(d.path);
    const auto found = expand_locals({root + "\\*.jpg", root + "\\c.txt", root + "\\sub\\"});
    REQUIRE_MESSAGE(found.has_value(), found.error_or(""));
    std::vector<std::string> names;
    for (const auto& p : *found) {
        CHECK(p.is_absolute());
        names.push_back(text(p.filename()));
    }
    CHECK(names == std::vector<std::string>{"A.JPG", "b.jpg", "c.txt", "sub"});
    CHECK(expand_locals({root + "\\nope.txt"}).error_or("") == root + "\\nope.txt: no such file or folder");
    CHECK(expand_locals({root + "\\*.png"}).error_or("") == root + "\\*.png: no file or folder matches");
    CHECK(expand_locals({root + "\\s*\\x"}).error_or("") == root + "\\s*\\x: * and ? pick names in the last part only");
}

TEST_CASE("grab upload: a pattern leaves out links, junctions and grab's partial downloads, as a folder's upload does; named, they go") {
    Dir d("expand-left");
    const fs::path dl = d.path / "dl";
    put_local(dl, "a.txt", "a");
    put_local(dl, "x.mkv.grabpart", "part");
    put_local(dl, "x.mkv.grabpart.json", fetch::state_to_json(fetch::new_state("Main", "/x.mkv", 100, "2023-11-14T22:13:20Z", 100)));
    put_local(dl, "y.grabpart", "not grab's: no record"); // goes
    fs::create_directories(d.path / "outside");
    put_local(d.path / "outside", "o.txt", "o");
#ifdef _WIN32
    const std::wstring command = L"cmd /c mklink /J \"" + (dl / "linked").wstring() + L"\" \"" + (d.path / "outside").wstring() + L"\" >nul 2>nul";
    REQUIRE((_wsystem(command.c_str()) == 0 && fs::exists(dl / "linked")));
#else
    fs::create_directory_symlink(d.path / "outside", dl / "linked");
#endif
    const std::string root = text(dl);
    std::vector<std::string> notes;
    const auto found = expand_locals({root + "\\*"}, &notes);
    REQUIRE_MESSAGE(found.has_value(), found.error_or(""));
    std::vector<std::string> names;
    for (const auto& p : *found) names.push_back(text(p.filename()));
    CHECK(names == std::vector<std::string>{"a.txt", "y.grabpart"});
    CHECK(notes == std::vector<std::string>{root + "\\*: 1 link and 1 partial download left out (name them to send them)"});
    // Only those matched: said so, not "no file or folder matches".
    CHECK(expand_locals({root + "\\x.*"}).error_or("") ==
          root + "\\x.*: only 1 partial download matches, which grab leaves out (name them to send them)");
    CHECK(expand_locals({root + "\\l*"}).error_or("") == root + "\\l*: only 1 link matches, which grab leaves out (name them to send them)");
    // Named in full, each is taken as it is.
    notes.clear();
    const auto named = expand_locals({root + "\\linked", root + "\\x.mkv.grabpart"}, &notes);
    REQUIRE_MESSAGE(named.has_value(), named.error_or(""));
    CHECK(named->size() == 2);
    CHECK(notes.empty());
}

TEST_CASE("grab upload: the answers typed at the question about files already there") {
    using K = Typed::Kind;
    using E = upload::Existing;
    struct Case {
        std::string_view typed;
        bool one_by_one;
        std::optional<Typed> want;
    };
    for (const auto& c : {Case{.typed = "r", .one_by_one = true, .want = Typed{.kind = K::choice, .choice = E::replace, .rest = false}},
                          Case{.typed = " replace ", .one_by_one = false, .want = Typed{.kind = K::choice, .choice = E::replace, .rest = false}},
                          Case{.typed = "D", .one_by_one = false, .want = Typed{.kind = K::choice, .choice = E::different, .rest = true}},
                          Case{.typed = "newer", .one_by_one = false, .want = Typed{.kind = K::choice, .choice = E::newer, .rest = false}},
                          Case{.typed = "S", .one_by_one = false, .want = Typed{.kind = K::choice, .choice = E::skip, .rest = true}},
                          Case{.typed = "k", .one_by_one = false, .want = Typed{.kind = K::choice, .choice = E::keep_both, .rest = false}},
                          Case{.typed = "keep both", .one_by_one = false, .want = Typed{.kind = K::choice, .choice = E::keep_both, .rest = false}},
                          Case{.typed = "o", .one_by_one = true, .want = Typed{.kind = K::one_by_one, .choice = E::skip, .rest = false}},
                          Case{.typed = "o", .one_by_one = false, .want = std::nullopt},
                          Case{.typed = "Q", .one_by_one = false, .want = Typed{.kind = K::quit, .choice = E::skip, .rest = false}},
                          Case{.typed = "", .one_by_one = true, .want = std::nullopt},
                          Case{.typed = "x", .one_by_one = true, .want = std::nullopt}}) {
        CAPTURE(c.typed);
        const auto got = read_typed(c.typed, c.one_by_one);
        REQUIRE(got.has_value() == c.want.has_value());
        if (!got || !c.want) continue;
        CHECK(got->kind == c.want->kind);
        CHECK(got->choice == c.want->choice);
        CHECK(got->rest == c.want->rest);
    }
}

TEST_CASE("grab upload: the write check without writing (-n) looks only: the folder, read-only or not, its free space") {
    Server srv;
    srv.put("/up/file", "a file", local_time);
    const auto before = srv.tree();
    auto pool = srv.fake.pool(2);
    const auto there = upload::probe(*pool, "Fake", "/up", {}, false);
    REQUIRE_MESSAGE(there.has_value(), there.error_or(""));
    CHECK(there->writable);
    CHECK(there->why.empty());
    CHECK(there->free.value_or(0) > 0);
    const auto missing = upload::probe(*pool, "Fake", "/nope", {}, false);
    REQUIRE(missing.has_value());
    CHECK_FALSE(missing->writable);
    CHECK(missing->why == "there is no folder /nope on Fake");
    const auto file = upload::probe(*pool, "Fake", "/up/file", {}, false);
    REQUIRE(file.has_value());
    CHECK_FALSE(file->writable);
    CHECK(file->why == "there is no folder /up/file on Fake");
    pool.reset();
    for (const auto& r : srv.requests()) CHECK_MESSAGE((r == "STAT" || r == "EXTENDED"), r); // no OPEN: nothing made
    CHECK(srv.tree() == before);

    Server ro;
    ro.fake.read_only = true;
    ro.fake.read_only_as = sftp::Code::failure;
    auto ro_pool = ro.fake.pool(2);
    const auto refused = upload::probe(*ro_pool, "Fake", "/up", {}, false);
    REQUIRE(refused.has_value());
    CHECK_FALSE(refused->writable);
    CHECK(refused->why == "Fake is read-only for this account: nothing can be uploaded to /up");
}

TEST_CASE("grab upload: -n says what would go where and what is there already, and writes nothing on the server or here") {
    Dir d("dry");
    const fs::path a = put_local(d.path / "src", "a.txt", "aaa");
    const fs::path photos = d.path / "src" / "photos";
    put_local(photos, "1.jpg", "one");
    put_local(photos, "2.jpg", "two");
    Server srv;
    srv.put("/up/a.txt", "older", local_time - 3600);
    srv.folder("/up/photos");
    srv.put("/up/photos/1.jpg", "one", local_time);
    const auto server_before = srv.tree();
    const auto local_before = local_tree(d.path / "src");

    Run ask(srv, d, "ask.txt");
    CHECK(ask({"upload", text(a), text(photos), "/up", "-n"}) == 0);
    ask.close();
    CHECK(srv.tree() == server_before);
    CHECK(local_tree(d.path / "src") == local_before);
    for (const auto& r : srv.requests()) {
        CAPTURE(r);
        CHECK((r != "OPEN" && r != "WRITE" && r != "MKDIR" && r != "REMOVE" && r != "RENAME" && r != "SETSTAT" &&
               r != "FSETSTAT" && r != "RMDIR"));
    }
    const std::string said = ask.said();
    CAPTURE(said);
    CHECK(has(said, "grab: would upload file 1/2 " + text(a) + " (3 B)  ->  Fake:/up/a.txt  (up to 4 connections)"));
    CHECK(has(said, "grab: would upload folder 2/2 " + text(photos) +
                        " (2 files, 6 B; merged into the folder there)  ->  Fake:/up/photos  (up to 4 connections)"));
    CHECK(has(said, "grab: Fake:/up: a folder there, "));
    CHECK(has(said, "grab: 2 files already on Fake:/up: 1 the same, 1 newer here"));
    CHECK(has(said, "  a.txt  (yours: 3 B, "));
    CHECK(has(said, "; yours is newer): asked"));
    CHECK(has(said, "  photos/1.jpg  (yours: 3 B, "));
    CHECK(has(said, "; the same): skipped (the same)"));
    CHECK(has(said, "without a console to ask on it stops: to say what to do without being asked, add --existing"));
    CHECK(ask.prompts.empty());

    // --existing: what it would do with them.
    Run newer(srv, d, "newer.txt");
    CHECK(newer({"upload", text(a), text(photos), "/up", "-n", "--existing", "newer"}) == 0);
    newer.close();
    CHECK(has(newer.said(), "grab: --existing newer: 1 replaced, 1 skipped"));
    CHECK(srv.tree() == server_before);
}

TEST_CASE("grab upload: files and a folder go in byte for byte, the folder merged; the same command again sends nothing") {
    Dir d("send");
    const fs::path a = put_local(d.path / "src", "a.txt", "aaa");
    const fs::path photos = d.path / "src" / "photos";
    put_local(photos, "1.jpg", "one");
    put_local(photos, "sub/2.jpg", "two two");
    const std::string big = test::pattern(3 * MiB);
    put_local(photos, "big.bin", big);
    Server srv;
    srv.folder("/up/photos");
    srv.put("/up/photos/theirs.txt", "the server's own", local_time - 99);
    const auto local_before = local_tree(d.path / "src");

    {
        Run run(srv, d, "first.txt");
        CHECK(run({"upload", text(a), text(photos), "/up"}) == 0);
        run.close();
        const std::string said = run.said();
        CAPTURE(said);
        CHECK(has(said, "grab: file 1/2 " + text(a) + "  ->  Fake:/up/a.txt\n"));
        CHECK(has(said, "  Uploaded 1 file (3 B) to Fake:/up/a.txt in "));
        CHECK(has(said, "grab: folder 2/2 " + text(photos) + "  ->  Fake:/up/photos  (merged into the folder there)\n"));
        CHECK(has(said, "  Uploaded 3 files (3.0 MiB) to Fake:/up/photos in "));
        CHECK(has(said, "grab: 2 of 2 done\n"));
        CHECK(run.prompts.empty());
    }
    CHECK(srv.file("/up/a.txt") == "aaa");
    CHECK(srv.file("/up/photos/1.jpg") == "one");
    CHECK(srv.file("/up/photos/sub/2.jpg") == "two two");
    CHECK(srv.file("/up/photos/big.bin") == big);
    CHECK(srv.file("/up/photos/theirs.txt") == "the server's own");
    CHECK(srv.mtime("/up/a.txt") == local_time);
    CHECK(srv.mtime("/up/photos/big.bin") == local_time);
    CHECK(srv.temps("/up").first == 0);
    CHECK(srv.temps("/up/photos").first == 0);
    CHECK(local_tree(d.path / "src") == local_before);
    CHECK_FALSE(upload::saved_progress(d.records(), "Fake", "/up/photos/big.bin"));

    // Again, with no console and the setting Ask: everything there is the same, so nothing is
    // asked and nothing sent.
    srv.forget();
    const auto server_before = srv.tree();
    Run again(srv, d, "again.txt");
    CHECK(again({"upload", text(a), text(photos), "/up"}) == 0);
    again.close();
    CHECK(srv.written() == 0);
    CHECK(srv.tree() == server_before);
    CHECK(again.prompts.empty());
    const std::string said = again.said();
    CAPTURE(said);
    CHECK(has(said, "grab: 4 files already on Fake:/up and the same (size and time): skipped"));
    CHECK(has(said, "  skipped: Fake:/up/a.txt is there already"));
    CHECK(has(said, "  nothing uploaded: all 3 files are already on Fake, skipped"));

    // --existing skip says what it does, and sends nothing either.
    srv.forget();
    Run skip(srv, d, "skip.txt");
    CHECK(skip({"upload", text(a), "/up", "--existing", "skip"}) == 0);
    skip.close();
    CHECK(srv.written() == 0);
    CHECK(has(skip.said(), "grab: 1 file already on Fake:/up: 1 the same; --existing skip: nothing replaced, 1 skipped"));
}

TEST_CASE("grab upload: files there that differ are asked about on the console, for all or one by one; with none, nothing is replaced") {
    Dir d("ask");
    const fs::path f = d.path / "src" / "f";
    put_local(f, "x.txt", "new x");
    put_local(f, "y.txt", "new y");
    put_local(f, "z.txt", "new z");
    const fs::path single = put_local(d.path / "src", "a.txt", "new a");
    const auto prepared = [](Server& srv) {
        srv.folder("/up/f");
        srv.put("/up/f/x.txt", "old x!", local_time - 100);
        srv.put("/up/f/y.txt", "old y!", local_time - 100);
        srv.put("/up/f/z.txt", "old z!", local_time - 100);
        srv.put("/up/a.txt", "old a!", local_time - 100);
    };
    const std::vector<std::string> argv{"upload", text(f), "/up"};

    SUBCASE("no console: an error naming --existing, nothing replaced") {
        Server srv;
        prepared(srv);
        const auto before = srv.tree();
        Run run(srv, d, "none.txt");
        CHECK(run(argv) == exit_upload_failed);
        run.close();
        CHECK(srv.tree() == before);
        CHECK(run.prompts.empty());
        const std::string said = run.said();
        CAPTURE(said);
        CHECK(has(said, "grab: 3 files already on Fake:/up: 3 newer here\n"));
        CHECK(has(said, "  f/x.txt  (yours: 5 B, "));
        CHECK(has(said, "grab: error: nothing there was replaced; to say what to do without being asked, add --existing "
                        "replace, different, newer, skip or both"));
    }
    SUBCASE("for all: skip, then replace") {
        Server srv;
        prepared(srv);
        const auto before = srv.tree();
        Run skip(srv, d, "skip.txt");
        skip.s.console = true;
        skip.answers = {"s"};
        CHECK(skip(argv) == 0);
        skip.close();
        CHECK(srv.tree() == before);
        REQUIRE(skip.prompts.size() == 1);
        CHECK(skip.prompts[0] == "[r]eplace, replace if [d]ifferent, replace if [n]ewer, [s]kip, [k]eep both, [o]ne by one, [q]uit: ");
        Run replace(srv, d, "replace.txt");
        replace.s.console = true;
        replace.answers = {"what", "r"}; // not an answer: asked again
        CHECK(replace(argv) == 0);
        replace.close();
        CHECK(replace.prompts.size() == 2);
        CHECK(srv.file("/up/f/x.txt") == "new x");
        CHECK(srv.file("/up/f/z.txt") == "new z");
    }
    SUBCASE("one by one: a capital letter answers the rest") {
        Server srv;
        prepared(srv);
        Run run(srv, d, "each.txt");
        run.s.console = true;
        run.answers = {"o", "k", "R"};
        CHECK(run(argv) == 0);
        run.close();
        CHECK(run.prompts.size() == 3); // the question, then x and y; R went for z too
        CHECK(run.prompts[1] == "  [r]eplace, if [d]ifferent, if [n]ewer, [s]kip, [k]eep both, [q]uit (R, D, N, S, K: the rest too): ");
        CHECK(srv.file("/up/f/x.txt") == "old x!");
        CHECK(srv.file("/up/f/x (2).txt") == "new x");
        CHECK(srv.file("/up/f/y.txt") == "new y");
        CHECK(srv.file("/up/f/z.txt") == "new z");
        CHECK_FALSE(srv.file("/up/f/z (2).txt"));
    }
    SUBCASE("quit: exit 3, nothing sent; a single file: Keep both, said where it went") {
        Server srv;
        prepared(srv);
        const auto before = srv.tree();
        Run quit(srv, d, "quit.txt");
        quit.s.console = true;
        quit.answers = {"q"};
        CHECK(quit({"upload", text(f), text(single), "/up"}) == exit_upload_not_found);
        quit.close();
        CHECK(srv.tree() == before);
        Run both(srv, d, "both.txt");
        both.s.console = true;
        both.answers = {"k"};
        CHECK(both({"upload", text(single), "/up"}) == 0);
        both.close();
        REQUIRE(both.prompts.size() == 1);
        CHECK(both.prompts[0] == "[r]eplace, replace if [d]ifferent, replace if [n]ewer, [s]kip, [k]eep both, [q]uit: ");
        CHECK(srv.file("/up/a.txt") == "old a!");
        CHECK(srv.file("/up/a (2).txt") == "new a");
        CHECK(has(both.said(), "  Uploaded 1 file (5 B) to Fake:/up/a (2).txt in "));
    }
}

TEST_CASE("grab upload: a read-only server, a REMOTE_DIR that isn't a folder there end with their codes and plain words, nothing written") {
    Dir d("refused");
    const fs::path a = put_local(d.path / "src", "a.txt", "aaa");
    {
        Server srv;
        srv.fake.read_only = true;
        srv.fake.read_only_as = sftp::Code::failure; // as Box.Guest answers
        const auto before = srv.tree();
        Run run(srv, d, "ro.txt");
        CHECK(run({"upload", text(a), "/up"}) == exit_upload_failed);
        Run dry(srv, d, "ro-dry.txt");
        CHECK(dry({"upload", text(a), "/up", "-n"}) == exit_upload_failed);
        run.close();
        dry.close();
        CHECK(srv.tree() == before);
        CHECK(has(run.said(), "grab: error: Fake is read-only for this account: nothing can be uploaded to /up\n"));
        CHECK(has(dry.said(), "grab: error: Fake is read-only for this account: nothing can be uploaded to /up\n"));
    }
    {
        Server srv;
        srv.fake.statvfs = false;
        srv.fake.read_only = true; // "permission denied", and no statvfs to say more
        Run run(srv, d, "denied.txt");
        CHECK(run({"upload", text(a), "/up"}) == exit_upload_failed);
        run.close();
        CHECK(has(run.said(), "grab: error: Fake doesn't let this account write in /up\n"));
        CHECK_FALSE(srv.file("/up/a.txt"));
    }
    {
        // A server with no OpenSSH extensions that answers only "Failure": its words, named.
        Server srv;
        srv.fake.statvfs = false;
        srv.fake.read_only = true;
        srv.fake.read_only_as = sftp::Code::failure;
        Run run(srv, d, "failure.txt");
        CHECK(run({"upload", text(a), "/up"}) == exit_upload_failed);
        run.close();
        CHECK(has(run.said(), "grab: error: Fake refused a file in /up: Failure\n"));
        CHECK_FALSE(srv.file("/up/a.txt"));
    }
    {
        Server srv;
        srv.put("/up/file", "a file", local_time);
        const auto before = srv.tree();
        Run missing(srv, d, "missing.txt");
        CHECK(missing({"upload", text(a), "/nope"}) == exit_upload_not_found);
        Run file(srv, d, "file.txt");
        CHECK(file({"upload", text(a), "/up/file/"}) == exit_upload_not_found);
        missing.close();
        file.close();
        CHECK(srv.tree() == before);
        CHECK(has(missing.said(), "grab: error: there is no folder /nope on Fake\n"));
        CHECK(has(file.said(), "grab: error: /up/file on Fake is a file, not a folder\n"));
    }
}

TEST_CASE("grab upload: Ctrl+C keeps a big file's partial upload; the same command again goes on from it, files sent already skipped unasked") {
    Dir d("resume");
    const fs::path f = d.path / "src" / "f";
    for (int i = 1; i <= 5; ++i) put_local(f, std::format("{}.txt", i), std::format("small {}", i));
    const std::string big = test::pattern(8 * MiB);
    put_local(f, "big.bin", big);
    Server srv;
    const std::vector<std::string> argv{"upload", text(f), "/up"};

    std::uint64_t saved_done = 0;
    {
        // One connection under 1 MiB/s: the small files first, then the big one's bytes in order.
        Run first(srv, d, "first.txt", 1);
        first.limiter.set_rate(MiB);
        std::stop_source stop;
        // Ctrl+C once the small files are in place and the big one's temp holds 2 MiB (at most a
        // quarter second's worth of it unacknowledged under the limit: fetch::limited_in_flight).
        std::jthread pressed([&](const std::stop_token& done) {
            while (!done.stop_requested()) {
                if (srv.file("/up/f/5.txt") && srv.temps("/up/f").second >= 2 * MiB) {
                    stop.request_stop();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        CHECK(first(argv, stop.get_token()) == proc::exit_stopped);
        pressed.request_stop();
        pressed.join();
        first.close();
        CHECK(has(first.said(), "files done; run the same command again to continue\n"));
        const auto saved = upload::saved_progress(d.records(), "Fake", "/up/f/big.bin");
        REQUIRE(saved.has_value());
        saved_done = saved.value_or(std::pair<std::uint64_t, std::uint64_t>{}).first;
        CHECK(saved_done >= MiB);
        CHECK(saved_done < big.size());
        CHECK(srv.temps("/up/f").first == 1); // the big file's, kept with its record
        CHECK_FALSE(srv.file("/up/f/big.bin"));
    }
    srv.forget();
    Run again(srv, d, "again.txt");
    CHECK(again(argv) == 0);
    again.close();
    CHECK(srv.written() == big.size() - saved_done); // nothing acknowledged was sent again
    CHECK(srv.file("/up/f/big.bin") == big);
    for (int i = 1; i <= 5; ++i) CHECK(srv.file(std::format("/up/f/{}.txt", i)) == std::format("small {}", i));
    CHECK(srv.temps("/up/f").first == 0);
    CHECK_FALSE(upload::saved_progress(d.records(), "Fake", "/up/f/big.bin"));
    CHECK(again.prompts.empty());
    const std::string said = again.said();
    CAPTURE(said);
    CHECK(has(said, "grab: 5 files already on Fake:/up and the same (size and time): skipped"));
    CHECK(has(said, "  Uploaded 1 file (8.0 MiB, "));
    CHECK(has(said, "; 5 already there, skipped\n"));
}

TEST_CASE("grab upload: after Ctrl+C the same command again goes on under Keep both too, with no second copies; under Replace it names --existing different") {
    Dir d("rerun");
    const fs::path f = d.path / "src" / "f";
    for (int i = 1; i <= 5; ++i) put_local(f, std::format("{}.txt", i), std::format("small {}", i));
    const std::string big = test::pattern(8 * MiB);
    put_local(f, "big.bin", big);
    // A first run with `existing` stopped as the resume case's is: what it said, and the bytes of
    // the big file its record has.
    const auto stopped = [&](Server& srv, const std::string& existing, std::uint64_t& saved_done) {
        Run first(srv, d, existing + "-first.txt", 1);
        first.limiter.set_rate(MiB);
        std::stop_source stop;
        std::jthread pressed([&](const std::stop_token& done) {
            while (!done.stop_requested()) {
                if (srv.file("/up/f/5.txt") && srv.temps("/up/f").second >= 2 * MiB) {
                    stop.request_stop();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        CHECK(first({"upload", text(f), "/up", "--existing", existing}, stop.get_token()) == proc::exit_stopped);
        pressed.request_stop();
        pressed.join();
        first.close();
        const auto saved = upload::saved_progress(d.records(), "Fake", "/up/f/big.bin");
        REQUIRE(saved.has_value());
        saved_done = saved.value_or(std::pair<std::uint64_t, std::uint64_t>{}).first;
        return first.said();
    };
    const auto sent_whole = [&](Server& srv) {
        CHECK(srv.file("/up/f/big.bin") == big);
        for (int i = 1; i <= 5; ++i) CHECK(srv.file(std::format("/up/f/{}.txt", i)) == std::format("small {}", i));
        std::scoped_lock lock(srv.fake.store.mutex);
        for (const auto& name : srv.fake.store.dirs["/up/f"]) {
            CAPTURE(name);
            CHECK(name.find(" (2)") == std::string::npos);
        }
    };

    SUBCASE("Keep both: the files that got there are the same, so skipped, not sent as second copies") {
        Server srv;
        std::uint64_t saved_done = 0;
        CHECK(has(stopped(srv, "both", saved_done), "files done; run the same command again to continue\n"));
        srv.forget();
        Run again(srv, d, "both-again.txt");
        CHECK(again({"upload", text(f), "/up", "--existing", "both"}) == 0);
        again.close();
        CHECK(srv.written() == big.size() - saved_done); // only the rest
        sent_whole(srv);
        const std::string said = again.said();
        CAPTURE(said);
        CHECK(has(said, "grab: 5 files already on Fake:/up: 5 the same; --existing both: nothing replaced, 5 skipped\n"));
        // -n says the same.
        Run dry(srv, d, "both-dry.txt");
        CHECK(dry({"upload", text(f), "/up", "--existing", "both", "-n"}) == 0);
        dry.close();
        CHECK(has(dry.said(), "; the same): skipped (the same)\n"));
        CHECK(has(dry.said(), "grab: --existing both: nothing replaced, 6 skipped\n"));
    }
    SUBCASE("Replace: the same command again would send them again, so it says --existing different") {
        Server srv;
        std::uint64_t saved_done = 0;
        const std::string first = stopped(srv, "replace", saved_done);
        CAPTURE(first);
        CHECK(has(first, "files done; run it again with --existing different to continue (--existing replace would send the 5 "
                         "files already there again)\n"));
        srv.forget();
        Run again(srv, d, "replace-again.txt");
        CHECK(again({"upload", text(f), "/up", "--existing", "different"}) == 0);
        again.close();
        CHECK(srv.written() == big.size() - saved_done);
        sent_whole(srv);
    }
}

TEST_CASE("grab upload: a pattern's files go together as a folder's do; a Keep-both copy takes no name another pick goes to") {
    Dir d("batch");
    const fs::path src = d.path / "src";
    constexpr int notes = 20;
    for (int i = 0; i < notes; ++i) put_local(src, std::format("n{:02}.txt", i), std::format("note {}", i));
    put_local(src, "a.txt", "new a");
    const fs::path other = put_local(d.path / "other", "a (2).txt", "the other pick");
    Server srv;
    srv.put("/up/a.txt", "old a!", local_time - 100);
    Run run(srv, d, "batch.txt");
    CHECK(run({"upload", text(src) + "\\*.txt", text(other), "/up", "--existing", "both"}) == 0);
    run.close();
    const std::string said = run.said();
    CAPTURE(said);
    CHECK(has(said, std::format("grab: 21 files from {}  ->  Fake:/up  (picks 1-21/22)\n", text(src))));
    CHECK(has(said, "  Uploaded 21 files ("));
    CHECK(has(said, "grab: file 22/22 " + text(other) + "  ->  Fake:/up/a (2).txt\n"));
    CHECK(has(said, "grab: 22 of 22 done\n"));
    CHECK(srv.file("/up/a.txt") == "old a!");
    CHECK(srv.file("/up/a (3).txt") == "new a"); // "a (2).txt" is the other pick's
    CHECK(srv.file("/up/a (2).txt") == "the other pick");
    for (int i = 0; i < notes; ++i) {
        CHECK(srv.file(std::format("/up/n{:02}.txt", i)) == std::format("note {}", i));
        CHECK(srv.mtime(std::format("/up/n{:02}.txt", i)) == local_time);
    }
    CHECK(srv.temps("/up").first == 0);
    // The batch's files go as a folder's small files do (no LSTAT of each target before it is
    // sent, as a file alone has: upload::run), beside the look's one LSTAT a pick.
    const auto requests = srv.requests();
    const auto lstats = static_cast<std::size_t>(std::ranges::count(requests, std::string("LSTAT")));
    MESSAGE("LSTAT requests: " << lstats);
    CHECK(lstats < 22 + 2 * 21); // 68 with each file alone
}

TEST_CASE("grab upload: a server copy verify finds different gets the command that sends it alone; a second copy is named, never replaced") {
    Dir d("differs");
    const fs::path f = d.path / "src" / "f";
    const fs::path a = put_local(f, "a.txt", "alpha");
    put_local(f, "b.txt", "bravo");
    put_local(f, "c.txt", "charlie");
    Server srv;
    srv.folder("/up/f");
    srv.put("/up/f/b.txt", "old b!", local_time - 100);
    // The server's md5sum sees a.txt's copy and b.txt's second copy damaged.
    test::Box box;
    box.files = {{"/up/f/a.txt", "alpHa"}, {"/up/f/b.txt", "old b!"}, {"/up/f/b (2).txt", "brAvo"}, {"/up/f/c.txt", "charlie"}};
    for (const auto& [path, content] : box.files) box.mtimes[path] = static_cast<std::uint32_t>(local_time);
    engine::Context ctx;
    ctx.settings.name = "Fake";
    ctx.remote.host = "fake.example";
    Run run(srv, d, "differs.txt");
    run.s.ctx = &ctx;
    run.s.verify = true;
    run.s.verify_pool = box.pool(4, 1);
    CHECK(run({"upload", text(f), "/up", "--existing", "both"}) == exit_upload_failed);
    run.close();
    const std::string said = run.said();
    CAPTURE(said);
    CHECK(has(said, "grab: your files are as they were; to send them again:\n"));
    CHECK(has(said, "  grab upload \"" + text(a) + "\" -r Fake \"/up/f\" --existing replace\n"));
    CHECK(has(said, "  Fake:/up/f/b (2).txt (a second copy, Keep both): no rerun can replace that name, and --existing replace "
                    "would replace the file you kept; remove it there, then send the file again\n"));
    CHECK_FALSE(has(said, "the other files are on Fake;")); // nothing else failed: no rerun of everything
    CHECK(srv.file("/up/f/b.txt") == "old b!");
}

namespace {

// A file of someone else's put on the fake server from inside its OPEN hook (the store's lock held).
void put_in(test::FakeFiles& st, const std::string& path, const std::string& content) {
    st.files[path] = content;
    st.mtimes[path] = static_cast<std::uint32_t>(local_time - 100);
    const auto slash = path.rfind('/');
    st.dirs[slash == 0 ? "/" : path.substr(0, slash)].push_back(path.substr(slash + 1));
}

// Someone else's file turns up at dir/name for each of `names` once grab opens its temp for it:
// after the look, before the name is taken (a late clash).
void turns_up(Server& srv, const std::string& dir, const std::vector<std::string>& names) {
    srv.fake.on_open = [dir, names, done = std::make_shared<std::set<std::string>>()](test::FakeFiles& st, const std::string& path) {
        for (const auto& n : names) {
            if (path.starts_with(dir + "/." + n + ".") && done->insert(n).second) put_in(st, dir + "/" + n, "theirs " + n);
        }
    };
}

} // namespace

TEST_CASE("grab upload: a file that turns up at its name during the run is asked about then, a single file and a folder's") {
    Dir d("late");
    const fs::path single = put_local(d.path / "src", "a.txt", "new a");
    const fs::path f = d.path / "src" / "f";
    for (const char* n : {"x.txt", "y.txt", "z.txt"}) put_local(f, n, std::string("new ") + n);

    SUBCASE("a single file: Keep both, said where it went") {
        Server srv;
        turns_up(srv, "/up", {"a.txt"});
        Run run(srv, d, "single-k.txt");
        run.s.console = true;
        run.answers = {"k"};
        CHECK(run({"upload", text(single), "/up"}) == 0);
        run.close();
        CHECK(run.prompts.size() == 1);
        CHECK(srv.file("/up/a.txt") == "theirs a.txt");
        CHECK(srv.file("/up/a (2).txt") == "new a");
        CHECK(srv.temps("/up").first == 0);
        CHECK(has(run.said(), "  Uploaded 1 file (5 B) to Fake:/up/a (2).txt in "));
    }
    SUBCASE("a single file, no console: exit 6 naming --existing, theirs kept") {
        Server srv;
        turns_up(srv, "/up", {"a.txt"});
        Run run(srv, d, "single-none.txt");
        CHECK(run({"upload", text(single), "/up"}) == exit_upload_failed);
        run.close();
        CHECK(srv.file("/up/a.txt") == "theirs a.txt");
        CHECK_FALSE(srv.file("/up/a (2).txt"));
        CHECK(srv.temps("/up").first == 0);
        CHECK(has(run.said(), "grab: error: nothing there was replaced; to say what to do without being asked, add --existing"));
    }
    SUBCASE("a single file: q is exit 3; Ctrl+C at the question is 130") {
        Server srv;
        turns_up(srv, "/up", {"a.txt"});
        Run quit(srv, d, "single-q.txt");
        quit.s.console = true;
        quit.answers = {"q"};
        CHECK(quit({"upload", text(single), "/up"}) == exit_upload_not_found);
        quit.close();
        CHECK(srv.file("/up/a.txt") == "theirs a.txt");

        Server srv2;
        turns_up(srv2, "/up", {"a.txt"});
        Run pressed(srv2, d, "single-c.txt");
        pressed.s.console = true;
        std::stop_source stop;
        pressed.s.ask = [&stop](const std::string&) -> std::optional<std::string> {
            stop.request_stop(); // Ctrl+C at the prompt: the input ends
            return std::nullopt;
        };
        CHECK(pressed({"upload", text(single), "/up"}, stop.get_token()) == proc::exit_stopped);
        pressed.close();
        CHECK(srv2.file("/up/a.txt") == "theirs a.txt");
    }
    SUBCASE("a folder, one by one: each answer goes to its own file") {
        Server srv;
        turns_up(srv, "/up/f", {"y.txt", "z.txt"});
        Run run(srv, d, "folder-o.txt");
        run.s.console = true;
        run.answers = {"o", "s", "r"}; // y skipped, z replaced
        CHECK(run({"upload", text(f), "/up"}) == 0);
        run.close();
        CHECK(run.prompts.size() == 3);
        CHECK(srv.file("/up/f/x.txt") == "new x.txt");
        CHECK(srv.file("/up/f/y.txt") == "theirs y.txt");
        CHECK(srv.file("/up/f/z.txt") == "new z.txt");
        CHECK(srv.temps("/up/f").first == 0);
        CHECK(has(run.said(), "grab: 2 files already on Fake:/up/f: 2 newer here\n"));
        CHECK(has(run.said(), "  f/y.txt  (yours: "));
    }
    SUBCASE("a folder, no console: exit 6, the others sent, theirs kept, the hint said") {
        Server srv;
        turns_up(srv, "/up/f", {"y.txt"});
        Run run(srv, d, "folder-none.txt");
        CHECK(run({"upload", text(f), "/up"}) == exit_upload_failed);
        run.close();
        CHECK(srv.file("/up/f/x.txt") == "new x.txt");
        CHECK(srv.file("/up/f/y.txt") == "theirs y.txt");
        CHECK(srv.file("/up/f/z.txt") == "new z.txt");
        CHECK(has(run.said(), "grab: to say what to do without being asked, add --existing"));
    }
    SUBCASE("a folder: q is exit 3") {
        Server srv;
        turns_up(srv, "/up/f", {"y.txt"});
        Run run(srv, d, "folder-q.txt");
        run.s.console = true;
        run.answers = {"q"};
        CHECK(run({"upload", text(f), "/up"}) == exit_upload_not_found);
        run.close();
        CHECK(srv.file("/up/f/y.txt") == "theirs y.txt");
    }
}
