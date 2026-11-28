// Quick access's pins (pins.hpp; PINS.md, chunk 1): the sealed file in state_dir, its changes one
// at a time under the settings lock (the runner's scratch APPDATA, test_main.cpp), and how pins
// follow grab's own renames, moves and deletes. Every path is made up.

#include "pins.hpp"
#include "server_files.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <windows.h> // IWYU pragma: keep (umbrella header for the Win32 API)

#include <cstddef>
#include <cstdio>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using namespace grab;
using pins::Moved;
using pins::Pin;
using pins::Pins;

namespace {

constexpr std::string_view sealed_header = "grab-pins-sealed 1\n";
constexpr std::string_view sealed_purpose = "grab pinned folders";

// A folder of its own for each case, removed afterwards.
struct Dir {
    std::filesystem::path dir;
    std::filesystem::path file;
    explicit Dir(const std::string& name) {
        dir = std::filesystem::temp_directory_path() / name;
        file = dir / "pins";
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir);
    }
    ~Dir() {
        try {
            std::error_code ec;
            std::filesystem::remove_all(dir, ec);
        } catch (...) { // only std::bad_alloc, which a destructor must not throw
            std::fputs("grab_tests: a temp folder was left behind\n", stderr);
        }
    }
    Dir(const Dir&) = delete;
    Dir& operator=(const Dir&) = delete;
    // The files moved aside ("pins.bad-..."), with what they hold.
    [[nodiscard]] std::vector<std::string> aside() const {
        std::vector<std::string> out;
        for (const auto& e : std::filesystem::directory_iterator(dir)) {
            if (util::path_to_utf8(e.path().filename()).starts_with("pins.bad-")) out.push_back(util::read_file(e.path()).value_or("?"));
        }
        return out;
    }
};

Pin pin(std::string path, std::string name = {}) { return Pin{.path = std::move(path), .name = std::move(name)}; }

// A run that ended early, as server_files says it.
server_files::Report ended(bool stopped, std::string error) {
    server_files::Report r;
    r.stopped = stopped;
    r.error = std::move(error);
    return r;
}

// `json` as grab seals the file.
std::string sealed(std::string_view json, std::string_view purpose = sealed_purpose) {
    const auto data = util::protect_for_user(json, purpose);
    REQUIRE(data.has_value());
    return std::string(sealed_header) + data.value_or("");
}

// One change to the file: an add, which must work.
void add_to(const std::filesystem::path& file, const std::string& server, const std::string& path, const std::string& name = {}) {
    const auto done = pins::update(file, [&](Pins& p) { return pins::add(p, server, path, name).transform([] { return true; }); });
    REQUIRE_MESSAGE(done.error.empty(), done.error);
    CHECK(done.changed);
}

std::vector<std::string> paths_of(const Pins& p, const std::string& server) {
    std::vector<std::string> out;
    if (const auto it = p.find(server); it != p.end()) {
        for (const Pin& pin : it->second) out.push_back(pin.path);
    }
    return out;
}

} // namespace

TEST_CASE("pins: the file round-trips, sealed; with no pin left it goes") {
    const Dir d("grab-test-pins-file");
    CHECK(pins::path_for(d.dir / "grab.conf").filename() == "pins");
    const auto none = pins::load(d.file);
    CHECK(none.pins.empty());
    CHECK(none.damaged.empty());
    CHECK_FALSE(std::filesystem::exists(d.file));

    add_to(d.file, "Main", "downloads/tv");
    add_to(d.file, "Main", "/srv/media/movies", "Movies");
    add_to(d.file, "Box", "sp ");
    add_to(d.file, "Box", "~");
    const auto back = pins::load(d.file);
    CHECK(back.damaged.empty());
    CHECK_FALSE(back.newer);
    CHECK(back.pins == Pins{{"Main", {pin("downloads/tv"), pin("/srv/media/movies", "Movies")}},
                            {"Box", {pin("sp "), pin("~")}}});
    // Sealed for this Windows user: none of it readable in the file's bytes.
    const auto bytes = util::read_file(d.file).value_or("");
    CHECK(bytes.starts_with(sealed_header));
    for (const char* plain : {"downloads", "movies", "Movies", "Main", "Box"}) CHECK(bytes.find(plain) == std::string::npos);

    // Unpinned one by one: the file goes with the last.
    for (const auto& [server, path] : std::vector<std::pair<std::string, std::string>>{
             {"Main", "downloads/tv"}, {"Main", "/srv/media/movies"}, {"Box", "sp "}, {"Box", "~"}}) {
        const auto done = pins::update(d.file, [&](Pins& p) -> std::expected<bool, std::string> { return pins::remove(p, server, path); });
        CHECK(done.changed);
    }
    CHECK_FALSE(std::filesystem::exists(d.file));
    CHECK(pins::load(d.file).pins.empty());
}

TEST_CASE("pins: a file that can't be read is moved aside, and the pins start empty") {
    const Dir d("grab-test-pins-damaged");
    const std::string good = R"({"version":1,"servers":{"Main":[{"path":"downloads/tv"}]}})";
    const std::vector<std::pair<const char*, std::string>> cases{
        {"plain text", good},
        {"cut off", sealed(good).substr(0, 40)},
        {"another account's (another purpose's) seal", sealed(good, "grab recent searches")},
        {"not JSON", sealed(R"({"version":1,"servers":)")},
        {"no version", sealed(R"({"servers":{"Main":[{"path":"downloads/tv"}]}})")},
    };
    for (const auto& [what, bytes] : cases) {
        CAPTURE(what);
        REQUIRE(util::write_file_atomic(d.file, bytes));
        const auto loaded = pins::load(d.file);
        CHECK(loaded.pins.empty());
        CHECK_FALSE(loaded.damaged.empty());
        REQUIRE_FALSE(loaded.aside.empty());
        CHECK(util::path_to_utf8(loaded.aside.filename()).starts_with("pins.bad-"));
        CHECK_FALSE(std::filesystem::exists(d.file));
        CHECK(util::read_file(loaded.aside).value_or("") == bytes); // kept as it was
        std::filesystem::remove(loaded.aside);
    }
    // A change finds one too: moved aside, the change made from none, and it says so.
    REQUIRE(util::write_file_atomic(d.file, "damaged"));
    const auto done = pins::update(d.file, [](Pins& p) { return pins::add(p, "Main", "x").transform([] { return true; }); });
    CHECK(done.changed);
    CHECK_FALSE(done.unread);
    CHECK(done.damaged == "it isn't a file of grab's pins");
    CHECK(util::path_to_utf8(done.aside.filename()).starts_with("pins.bad-"));
    CHECK(d.aside() == std::vector<std::string>{"damaged"});
    CHECK(paths_of(pins::load(d.file).pins, "Main") == std::vector<std::string>{"x"});
}

TEST_CASE("pins: a file that can't be read at all, or can't be moved aside, is left alone and a change refused") {
    const Dir d("grab-test-pins-held");
    add_to(d.file, "Main", "downloads/tv");
    const auto good = util::read_file(d.file).value_or("");
    // Another program holds it with no sharing: not read at all, its content maybe good.
    const auto hold = [&](DWORD share) {
        return CreateFileW(d.file.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    };
    HANDLE h = hold(0);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    const auto loaded = pins::load(d.file);
    CHECK_FALSE(loaded.damaged.empty());
    CHECK(loaded.aside.empty());
    const auto refused = pins::update(d.file, [](Pins& p) { return pins::add(p, "Main", "x").transform([] { return true; }); });
    CHECK_FALSE(refused.changed);
    CHECK(refused.unread);
    CHECK(refused.error.starts_with(pins::cant_save));
    CloseHandle(h);
    CHECK(util::read_file(d.file).value_or("") == good); // never replaced
    CHECK(d.aside().empty());
    CHECK(paths_of(pins::load(d.file).pins, "Main") == std::vector<std::string>{"downloads/tv"});

    // Damaged, but held so that it can't be moved aside (read, but no delete shared): kept, and
    // nothing written over it.
    REQUIRE(util::write_file_atomic(d.file, "damaged"));
    h = hold(FILE_SHARE_READ);
    REQUIRE(h != INVALID_HANDLE_VALUE);
    const auto damaged = pins::load(d.file);
    CHECK(damaged.damaged == "it isn't a file of grab's pins");
    CHECK(damaged.aside.empty());
    const auto kept = pins::update(d.file, [](Pins& p) { return pins::add(p, "Main", "x").transform([] { return true; }); });
    CHECK_FALSE(kept.changed);
    CHECK(kept.unread);
    CHECK(kept.error.starts_with("grab couldn't save Quick access: it isn't a file of grab's pins, and it couldn't be moved aside"));
    CloseHandle(h);
    CHECK(util::read_file(d.file).value_or("") == "damaged");
    CHECK(d.aside().empty());
}

TEST_CASE("pins: a newer grab's file is read as far as it can be and never written over") {
    const Dir d("grab-test-pins-newer");
    const std::string bytes =
        sealed(R"({"version":2,"servers":{"Main":[{"path":"downloads/tv","colour":"red"},{"path":"a","name":"A","kind":"x"}]},"groups":[]})");
    REQUIRE(util::write_file_atomic(d.file, bytes));
    const auto loaded = pins::load(d.file);
    CHECK(loaded.newer);
    CHECK(loaded.damaged.empty());
    CHECK(loaded.pins == Pins{{"Main", {pin("downloads/tv"), pin("a", "A")}}});
    // A change is refused, plainly, and the file stays byte for byte.
    auto done = pins::update(d.file, [](Pins& p) { return pins::add(p, "Main", "b").transform([] { return true; }); });
    CHECK_FALSE(done.changed);
    CHECK(done.error == pins::newer_text);
    CHECK(done.pins == loaded.pins);
    done = pins::update(d.file, [](Pins& p) -> std::expected<bool, std::string> { return pins::forget_server(p, "Main"); });
    CHECK(done.error == pins::newer_text);
    CHECK(util::read_file(d.file).value_or("") == bytes);
    // One that changes nothing says nothing.
    done = pins::update(d.file, [](Pins& p) -> std::expected<bool, std::string> { return pins::forget_server(p, "Box"); });
    CHECK(done.error.empty());
    CHECK_FALSE(done.changed);
    CHECK(d.aside().empty());
}

TEST_CASE("pins: entries that can't be pins are left out; a server's past 100 and a path twice too") {
    const Dir d("grab-test-pins-entries");
    std::string many;
    for (int i = 0; i < 105; ++i) many += std::format(R"({}{{"path":"d{}"}})", i == 0 ? "" : ",", i);
    const std::string long_name(256, 'n');
    REQUIRE(util::write_file_atomic(
        d.file, sealed(std::format(R"({{"version":1,"servers":{{"Main":[{{"path":"ok"}},{{"name":"no path"}},{{"path":""}},)"
                                   R"({{"path":"a\u0001b"}},{{"path":7}},"x",{{"path":"ok"}},{{"path":"named","name":"{}"}},)"
                                   R"({{"path":"tab","name":"a\tb"}}],"":[{{"path":"x"}}],"Empty":[],"Box":[{}],"Bad":"x"}}}})",
                                   long_name, many))));
    const auto loaded = pins::load(d.file);
    CHECK(loaded.damaged.empty());
    // The name that can't be one dropped, the pin kept.
    CHECK(loaded.pins.at("Main") == std::vector<Pin>{pin("ok"), pin("named"), pin("tab")});
    CHECK(loaded.pins.at("Box").size() == pins::max_per_server);
    CHECK(loaded.pins.at("Box").back().path == "d99");
    CHECK(loaded.pins.size() == 2);
}

TEST_CASE("pins: two writers take the lock in turn, each from the file as it is then") {
    const Dir d("grab-test-pins-writers");
    constexpr int each = 40;
    const auto writer = [&](const std::string& server) {
        for (int i = 0; i < each; ++i) {
            const auto done = pins::update(d.file, [&](Pins& p) {
                return pins::add(p, server, std::format("folder {}", i)).transform([] { return true; });
            });
            CHECK(done.error.empty());
        }
    };
    {
        std::jthread a(writer, "Main");
        std::jthread b(writer, "Box");
    }
    const auto loaded = pins::load(d.file);
    REQUIRE(loaded.pins.contains("Main"));
    REQUIRE(loaded.pins.contains("Box"));
    // None lost: each change started from the other's.
    CHECK(loaded.pins.at("Main").size() == each);
    CHECK(loaded.pins.at("Box").size() == each);
}

TEST_CASE("pins: add last or at Undo's place; the same folder however spelled; 100 per server; the limits") {
    Pins p;
    REQUIRE(pins::add(p, "Main", "downloads/tv"));
    REQUIRE(pins::add(p, "Main", "/srv/movies", "Movies"));
    REQUIRE(pins::add(p, "Main", "a", {}, 0)); // Undo: back at its place
    REQUIRE(pins::add(p, "Main", "b", "B", 99)); // past the end: last
    CHECK(paths_of(p, "Main") == std::vector<std::string>{"a", "downloads/tv", "/srv/movies", "b"});
    CHECK(p.at("Main")[3].name == "B");
    // Pinned already: the same spelling, "." and a trailing '/', and with the login folder known
    // the absolute spelling. Named by the pin's own name, or its folder's.
    CHECK(pins::add(p, "Main", "downloads/tv").error_or("") == "tv is already in Quick access");
    CHECK(pins::add(p, "Main", "downloads/./tv/").error_or("") == "tv is already in Quick access");
    CHECK(pins::add(p, "Main", "/srv//movies").error_or("") == "Movies is already in Quick access");
    CHECK(pins::add(p, "Main", "/home/u/downloads/tv", {}, {}, "/home/u").error_or("") == "tv is already in Quick access");
    // Without the login folder the two spellings can't be compared: another pin.
    CHECK(pins::add(p, "Main", "/home/u/downloads/tv").has_value());
    // Another server's own.
    CHECK(pins::add(p, "Box", "downloads/tv").has_value());
    CHECK(paths_of(p, "Main").size() == 5);

    // 100 per server.
    Pins full;
    for (std::size_t i = 0; i < pins::max_per_server; ++i) REQUIRE(pins::add(full, "Main", std::format("d{}", i)));
    CHECK(pins::add(full, "Main", "one more").error_or("") == "Quick access holds 100 folders per server: unpin one first.");
    CHECK(pins::add(full, "Main", "one more", {}, 0).error_or("") == "Quick access holds 100 folders per server: unpin one first.");
    CHECK(pins::add(full, "Box", "one more").has_value());

    // A path: not a top (empty), at most 4 KiB, no control character or NUL, UTF-8.
    Pins q;
    CHECK(pins::add(q, "Main", "").error_or("") == "Open a folder of a server first.");
    CHECK(pins::add(q, "Main", std::string(pins::max_path_bytes + 1, 'a')).error_or("") ==
          "That folder's path is too long for Quick access (at most 4 KiB).");
    CHECK(pins::add(q, "Main", std::string(pins::max_path_bytes, 'a')).has_value());
    for (const std::string& bad : {std::string("a\nb"), std::string("a\0b", 3), std::string("a\x7F"), std::string("\x1F")}) {
        CHECK(pins::add(q, "Main", bad).error_or("") == "That folder's path holds a control character, so it can't be pinned.");
    }
    CHECK(pins::add(q, "Main", "caf\xE9").error_or("") == "That folder's path isn't UTF-8, so it can't be pinned.");
    CHECK(pins::add(q, "Main", "caf‛E9 ␊").has_value()); // grab's form of such names
    // A name: 1-255 characters (not bytes), no control characters (C0, DEL, C1).
    std::string georgian;
    for (int i = 0; i < 255; ++i) georgian += "ა"; // 3 bytes each
    CHECK(pins::add(q, "Main", "g", georgian).has_value());
    CHECK(pins::add(q, "Main", "g2", georgian + "ა").error_or("") == "A pin's name can be at most 255 characters.");
    for (const std::string& bad : {std::string("a\tb"), std::string("a\0b", 3), std::string("a\x7F"), std::string("a\xC2\x85")}) {
        CHECK(pins::add(q, "Main", "n", bad).error_or("") == "A pin's name can't hold control characters.");
    }
    CHECK(pins::add(q, "Main", "n", "\xFF").error_or("") == "A pin's name has to be text (UTF-8).");
    CHECK(pins::add(q, "Main", "n", "\xC2\xA0nbsp").has_value()); // U+00A0 isn't a control character
    CHECK(paths_of(q, "Main").size() == 4);
}

TEST_CASE("pins: unpin, rename (empty: the folder's own name) and move, at both ends") {
    Pins p;
    for (const char* path : {"a", "b/c", "/d", "e"}) REQUIRE(pins::add(p, "Main", path));
    // Rename: its own name, kept until emptied; a bad name refused.
    CHECK(pins::rename(p, "Main", "b/c", "Season 1").value_or(false));
    CHECK(pins::shown_name(p.at("Main")[1]) == "Season 1");
    CHECK_FALSE(pins::rename(p, "Main", "b/c", "Season 1").value_or(true)); // the same: no change
    CHECK(pins::rename(p, "Main", "b/./c", "x\n").error_or("") == "A pin's name can't hold control characters.");
    CHECK(pins::rename(p, "Main", "b/./c", "").value_or(false));
    CHECK(pins::shown_name(p.at("Main")[1]) == "c");
    CHECK_FALSE(pins::rename(p, "Main", "nope", "x").value_or(true));
    CHECK(pins::shown_name(pin("/")) == "/");
    CHECK(pins::shown_name(pin("/srv/x/")) == "x");

    // Move: to the top, to the end, past both ends (clamped), onto itself.
    CHECK(pins::move(p, "Main", "e", 0));
    CHECK(paths_of(p, "Main") == std::vector<std::string>{"e", "a", "b/c", "/d"});
    CHECK(pins::move(p, "Main", "e", 3));
    CHECK(paths_of(p, "Main") == std::vector<std::string>{"a", "b/c", "/d", "e"});
    CHECK(pins::move(p, "Main", "/d", -5));
    CHECK(paths_of(p, "Main") == std::vector<std::string>{"/d", "a", "b/c", "e"});
    CHECK(pins::move(p, "Main", "/d", 500));
    CHECK(paths_of(p, "Main") == std::vector<std::string>{"a", "b/c", "e", "/d"});
    CHECK_FALSE(pins::move(p, "Main", "/d", 3));
    CHECK(pins::move(p, "Main", "b/c", 1) == false);
    CHECK(pins::move(p, "Main", "b/c/", 0)); // however spelled
    CHECK(paths_of(p, "Main") == std::vector<std::string>{"b/c", "a", "e", "/d"});
    CHECK_FALSE(pins::move(p, "Box", "a", 0));

    // Unpin: however spelled; the server's entry goes with its last pin.
    CHECK(pins::remove(p, "Main", "./a"));
    CHECK_FALSE(pins::remove(p, "Main", "a"));
    for (const char* path : {"b/c", "e", "/d"}) CHECK(pins::remove(p, "Main", path));
    CHECK(p.empty());
}

TEST_CASE("pins: grab's renames and moves carry a pin and the pins below it; tv2 isn't below tv") {
    Pins p;
    for (const char* path : {"downloads/tv", "downloads/tv/Season 1", "downloads/tv2", "/home/u/downloads/tv/Season 2",
                             "downloads/tv/./Season 3/", "other"}) {
        REQUIRE(pins::add(p, "Main", path));
    }
    REQUIRE(pins::add(p, "Box", "downloads/tv"));
    REQUIRE(pins::rename(p, "Main", "downloads/tv/Season 1", "S1"));
    const Pins before = p;
    // Without the login folder: only the relative spellings follow.
    Pins q = p;
    CHECK(pins::follow(q, "Main", "", std::vector<Moved>{{.from = "downloads/tv", .to = "downloads/shows"}}));
    CHECK(paths_of(q, "Main") == std::vector<std::string>{"downloads/shows", "downloads/shows/Season 1", "downloads/tv2",
                                                          "/home/u/downloads/tv/Season 2", "downloads/shows/Season 3", "other"});
    // With it, the absolute spelling too, each with the new place's spelling and its own rest.
    CHECK(pins::follow(p, "Main", "/home/u", std::vector<Moved>{{.from = "downloads/tv", .to = "/srv/shows"}}));
    CHECK(paths_of(p, "Main") == std::vector<std::string>{"/srv/shows", "/srv/shows/Season 1", "downloads/tv2",
                                                          "/srv/shows/Season 2", "/srv/shows/Season 3", "other"});
    CHECK(p.at("Main")[1].name == "S1"); // its own name stays
    CHECK(p.at("Box") == before.at("Box")); // another server's pins stay
    // A trailing '/' on either side, and the absolute spelling of `from`.
    CHECK(pins::follow(p, "Main", "/home/u", std::vector<Moved>{{.from = "/home/u/other/", .to = "/srv/other/"}}));
    CHECK(paths_of(p, "Main").back() == "/srv/other");

    // Nothing to follow: a rename that changed nothing, however spelled; a place that didn't go;
    // a path no pin is at or below; another server.
    const Pins now = p;
    for (const Moved& m : std::vector<Moved>{{.from = "/srv/shows", .to = "/srv/shows"},
                                             {.from = "/srv/./shows", .to = "/srv/shows/"},
                                             {.from = "/srv/shows", .to = ""},
                                             {.from = "/srv/show", .to = "/srv/x"},
                                             {.from = "", .to = "x"},
                                             {.from = "/", .to = "/x"}}) {
        CAPTURE(m.from);
        CHECK_FALSE(pins::follow(p, "Main", "/home/u", std::vector<Moved>{m}));
    }
    CHECK_FALSE(pins::follow(p, "Gone", "", std::vector<Moved>{{.from = "a", .to = "b"}}));
    CHECK(p == now);

    // Two pins that end at one folder: the earlier stays, with its name.
    Pins two;
    REQUIRE(pins::add(two, "Main", "in/x"));
    REQUIRE(pins::add(two, "Main", "/home/u/x", "Mine"));
    REQUIRE(pins::add(two, "Main", "y"));
    CHECK(pins::follow(two, "Main", "/home/u", std::vector<Moved>{{.from = "in/x", .to = "x"}}));
    CHECK(two.at("Main") == std::vector<Pin>{pin("x"), pin("y")});
    // Two this change didn't move, spelled two ways (pinned before the login folder was known):
    // an unrelated rename leaves both, each with its own name.
    Pins apart;
    REQUIRE(pins::add(apart, "Main", "x"));
    REQUIRE(pins::add(apart, "Main", "/home/u/x", "Mine"));
    REQUIRE(pins::add(apart, "Main", "a"));
    CHECK(pins::follow(apart, "Main", "/home/u", std::vector<Moved>{{.from = "a", .to = "b"}}));
    CHECK(apart.at("Main") == std::vector<Pin>{pin("x"), pin("/home/u/x", "Mine"), pin("b")});
    // One of them moved onto the other's folder: the earlier stays.
    CHECK(pins::follow(apart, "Main", "/home/u", std::vector<Moved>{{.from = "b", .to = "x"}}));
    CHECK(apart.at("Main") == std::vector<Pin>{pin("x"), pin("/home/u/x", "Mine")});

    // Several moved at once, each by the deepest folder it is below.
    Pins many;
    for (const char* path : {"a", "a/b", "a/b/c", "d"}) REQUIRE(pins::add(many, "Main", path));
    CHECK(pins::follow(many, "Main", "", std::vector<Moved>{{.from = "a", .to = "z/a"}, {.from = "a/b", .to = "q"}, {.from = "d", .to = "z/d"}}));
    CHECK(paths_of(many, "Main") == std::vector<std::string>{"z/a", "q", "q/c", "z/d"});
}

TEST_CASE("pins: a move's pairs from each run: the places that went, not those that stayed or didn't go") {
    const std::vector<server_files::Item> items{{.from = "/a/tv", .to = "/b/tv", .choice = {}},
                                                {.from = "/a/f", .to = "/b/f", .choice = {}},
                                                {.from = "/b/x", .to = "/b/x", .choice = {}},
                                                {.from = "/a/y", .to = "/b/y", .choice = {}}};
    const std::vector<std::string> placed{"/b/tv (2)", "", "/b/x"}; // the last item has no place
    CHECK(pins::moved(items, placed) == std::vector<Moved>{{.from = "/a/tv", .to = "/b/tv (2)"}});
    // A paste: both runs' pairs, the first run's first; a copy's none.
    const std::vector<server_files::Item> again{{.from = "/a/f", .to = "/b/f", .choice = {}}};
    const std::vector<std::string> placed_again{"/b/f (2)"};
    CHECK(pins::moved_by_paste(false, items, placed, again, placed_again) ==
          std::vector<Moved>{{.from = "/a/tv", .to = "/b/tv (2)"}, {.from = "/a/f", .to = "/b/f (2)"}});
    CHECK(pins::moved_by_paste(false, items, placed, again, {}) == std::vector<Moved>{{.from = "/a/tv", .to = "/b/tv (2)"}});
    CHECK(pins::moved_by_paste(true, items, placed, again, placed_again).empty());
}

TEST_CASE("pins: a delete that ran to its end unpins what went and what was below it; one that ended early keeps all") {
    Pins p;
    for (const char* path : {"downloads/tv", "/home/u/downloads/tv/S1", "downloads/tv2", "x"}) REQUIRE(pins::add(p, "Main", path));
    REQUIRE(pins::add(p, "Box", "downloads/tv"));
    const std::vector<std::string> went{"downloads/tv/"};
    const Pins before = p;
    for (const auto& r : {ended(true, ""), ended(false, "Main doesn't let this account write in downloads/tv")}) {
        CHECK_FALSE(pins::deleted(p, "Main", "/home/u", went, r));
        CHECK(p == before);
    }
    CHECK(pins::deleted(p, "Main", "/home/u", went, ended(false, "")));
    CHECK(paths_of(p, "Main") == std::vector<std::string>{"downloads/tv2", "x"});
    CHECK(p.at("Box") == before.at("Box"));
    // The login folder and "/" never take every pin with them (grab refuses to delete them anyway).
    CHECK_FALSE(pins::drop_below(p, "Main", "", std::vector<std::string>{"", "/", "."}));
    CHECK(pins::drop_below(p, "Main", "", std::vector<std::string>{"downloads/tv2", "x"}));
    CHECK(p.size() == 1);
    CHECK(pins::forget_server(p, "Box"));
    CHECK_FALSE(pins::forget_server(p, "Box"));
    CHECK(p.empty());
}
