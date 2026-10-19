#include "browse.hpp"
#include "engine.hpp"
#include "file_list.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace grab;
using browse::Crumb;
using browse::Item;
using file_list::Entry;

namespace {

Entry dir(std::string path, std::optional<std::int64_t> modified = {}) {
    return Entry{std::move(path), true, std::nullopt, modified};
}
Entry file(std::string path, std::uint64_t size, std::optional<std::int64_t> modified = {}) {
    return Entry{std::move(path), false, size, modified};
}

std::shared_ptr<const file_list::List> list_of(std::vector<Entry> entries) {
    auto list = std::make_shared<file_list::List>();
    list->entries = std::move(entries);
    return list;
}

std::vector<std::string> names(const std::vector<Item>& items) {
    std::vector<std::string> out;
    for (const auto& i : items) out.push_back(i.name);
    return out;
}

} // namespace

TEST_CASE("folders come first, then files, in natural order ignoring case") {
    std::vector<Item> items{{"Episode 10.mkv", "x", false, 1, {}}, {"episode 2.mkv", "x", false, 1, {}},
                            {"b", "x", true, {}, {}},              {"Episode 1.mkv", "x", false, 1, {}},
                            {"A", "x", true, {}, {}},              {"ep 02.mkv", "x", false, 1, {}}};
    browse::sort_items(items);
    CHECK(names(items) == std::vector<std::string>{"A", "b", "ep 02.mkv", "Episode 1.mkv", "episode 2.mkv", "Episode 10.mkv"});
    CHECK(browse::natural_less("Season 9", "Season 10"));
    CHECK_FALSE(browse::natural_less("Season 10", "Season 9"));
    CHECK(browse::natural_less("a2", "a02") != browse::natural_less("a02", "a2")); // still a strict order
    CHECK_FALSE(browse::natural_less("same", "same"));
}

TEST_CASE("names in other scripts sort after digits and ASCII letters") {
    const std::string umlaut = "\xC3\x84pfel";     // Äpfel
    const std::string georgian = "\xE1\x83\x90.mkv"; // ა.mkv
    CHECK(browse::natural_less("2 Fast", umlaut));
    CHECK(browse::natural_less("zebra", umlaut));
    CHECK_FALSE(browse::natural_less(umlaut, "zebra"));
    CHECK(browse::natural_less(umlaut, georgian));
    std::vector<Item> items{{georgian, "x", false, 1, {}}, {"b.mkv", "x", false, 1, {}}, {umlaut, "x", false, 1, {}},
                            {"10.mkv", "x", false, 1, {}}, {"Z.mkv", "x", false, 1, {}}};
    browse::sort_items(items);
    CHECK(names(items) == std::vector<std::string>{"10.mkv", "b.mkv", "Z.mkv", umlaut, georgian});
}

TEST_CASE("what lies below a folder") {
    CHECK(browse::is_below("movies/a.avi", "movies"));
    CHECK(browse::is_below("movies/x/a.avi", "movies"));
    CHECK_FALSE(browse::is_below("movies", "movies"));
    CHECK_FALSE(browse::is_below("movies2/a", "movies"));
    CHECK(browse::is_below("movies", ""));  // the login home holds every relative path
    CHECK_FALSE(browse::is_below("/etc", ""));
    CHECK(browse::is_below("/etc", "/"));
    CHECK_FALSE(browse::is_below("/", "/"));
    CHECK(browse::root_of({"", "/srv"}, "/srv/x") == "/srv");
    CHECK(browse::root_of({"", "/srv"}, "tv/x") == "");
    CHECK_FALSE(browse::root_of({""}, "/etc").has_value());
}

TEST_CASE("a server's folders from its file list, down to the list's depth") {
    // The login home, max_depth 3: movies/georgian/2014 is in the list, its contents are not.
    const browse::Index index(list_of({dir("movies", 1697000000), dir("movies/georgian"), file("movies/georgian/b.avi", 50),
                                       file("movies/georgian/a.avi", 100, 1697354553), dir("movies/georgian/2014", 1600000000),
                                       dir("tv"), file("tv/x.mkv", 10), file("top.txt", 5, 1700000000)}),
                              {""}, 3);
    const auto top = index.top();
    REQUIRE(top.size() == 3);
    // Folders have no size, even when everything below them is known.
    CHECK(top[0] == Item{"movies", "movies", true, std::nullopt, 1, 1697000000});
    CHECK(top[1] == Item{"tv", "tv", true, std::nullopt, 1});
    CHECK(top[2] == Item{"top.txt", "top.txt", false, 5, std::nullopt, 1700000000});

    const auto georgian = index.folder("movies/georgian");
    REQUIRE(georgian.has_value());
    CHECK(*georgian == std::vector<Item>{{"2014", "movies/georgian/2014", true, std::nullopt, std::nullopt, 1600000000},
                                         {"a.avi", "movies/georgian/a.avi", false, 100, std::nullopt, 1697354553},
                                         {"b.avi", "movies/georgian/b.avi", false, 50, std::nullopt}});
    CHECK_FALSE(index.folder("movies/georgian/2014").has_value()); // listed live instead
    CHECK_FALSE(index.folder("missing").has_value());
    CHECK_FALSE(index.folder("/etc").has_value()); // outside the roots
    CHECK(index.folder("")->size() == 3);

    // One entry by its path, when the list holds its folder's contents.
    CHECK(index.entry("movies/georgian/a.avi") == Item{"a.avi", "movies/georgian/a.avi", false, 100, std::nullopt, 1697354553});
    CHECK(index.entry("movies/georgian/2014") ==
          Item{"2014", "movies/georgian/2014", true, std::nullopt, std::nullopt, 1600000000});
    CHECK(index.entry("tv")->dir);
    CHECK_FALSE(index.entry("movies/georgian/2014/x.avi").has_value()); // below the list's depth
    CHECK_FALSE(index.entry("movies/nope.avi").has_value());
    CHECK_FALSE(index.entry("").has_value());
    CHECK_FALSE(index.entry("/etc/passwd").has_value());
}

TEST_CASE("the files the list holds below a folder, for the free-space check") {
    // max_depth 3: movies/georgian/2014 is listed, its contents are not.
    const browse::Index index(list_of({dir("movies"), dir("movies/georgian"), file("movies/georgian/b.avi", 50),
                                       file("movies/georgian/a.avi", 100), dir("movies/georgian/2014"), dir("tv"),
                                       dir("tv/s1"), file("tv/s1/e1.mkv", 10), file("tv/s1/e2.mkv", 20), file("top.txt", 5)}),
                              {""}, 3);
    auto total = [](const browse::Index::Below& b) {
        std::uint64_t n = 0;
        for (const auto* e : b.files) n += e->size.value_or(0);
        return n;
    };
    const auto tv = index.below("tv");
    CHECK(tv.complete);
    CHECK(tv.files.size() == 2);
    CHECK(total(tv) == 30);
    const auto movies = index.below("movies"); // holds a folder deeper than the list
    CHECK_FALSE(movies.complete);
    CHECK(total(movies) == 150);
    const auto deep = index.below("movies/georgian/2014");
    CHECK_FALSE(deep.complete);
    CHECK(deep.files.empty());
    CHECK_FALSE(index.below("/etc").complete); // outside the roots
    CHECK(total(index.below("")) == 185);
}

TEST_CASE("several search roots are the server's top folders") {
    const browse::Index index(list_of({dir("/srv/media/a", 5), file("/srv/media/a/f.mkv", 7, 6), file("/home/x/n.txt", 1, 7)}),
                              {"/srv/media", "/home/x"}, 4);
    // The roots themselves are not in the list: no date.
    CHECK(index.top() == std::vector<Item>{{"/srv/media", "/srv/media", true, std::nullopt, 1},
                                           {"/home/x", "/home/x", true, std::nullopt, 1}});
    CHECK((*index.folder("/srv/media"))[0].modified == 5);
    CHECK(names(*index.folder("/srv/media/a")) == std::vector<std::string>{"f.mkv"});
    const browse::Index empty(list_of({}), {}, 4); // no list entries: the home is known and empty
    CHECK(empty.top().empty());
}

TEST_CASE("the path bar from the server's top") {
    CHECK(browse::crumbs({""}, "movies/georgian") ==
          std::vector<Crumb>{{"movies", "movies"}, {"georgian", "movies/georgian"}});
    CHECK(browse::crumbs({}, "").empty());
    CHECK(browse::crumbs({"/srv/media", "/home/x"}, "/srv/media/a/b") ==
          std::vector<Crumb>{{"/srv/media", "/srv/media"}, {"a", "/srv/media/a"}, {"b", "/srv/media/a/b"}});
    CHECK(browse::crumbs({"", "/srv"}, "") == std::vector<Crumb>{{"~", ""}});
    CHECK(browse::crumbs({"/"}, "/etc/x") == std::vector<Crumb>{{"etc", "/etc"}, {"x", "/etc/x"}});
    // Outside the roots: from "/", or "~" for a relative path.
    CHECK(browse::crumbs({""}, "/etc/ssh") == std::vector<Crumb>{{"/", "/"}, {"etc", "/etc"}, {"ssh", "/etc/ssh"}});
    CHECK(browse::crumbs({""}, "/") == std::vector<Crumb>{{"/", "/"}});
    CHECK(browse::crumbs({"/srv/media"}, "movies/x") ==
          std::vector<Crumb>{{"~", ""}, {"movies", "movies"}, {"x", "movies/x"}});
    CHECK(browse::crumbs({"/srv/media", "/home/x"}, "/etc") == std::vector<Crumb>{{"/", "/"}, {"etc", "/etc"}});
}

TEST_CASE("a path typed in the address bar") {
    CHECK(browse::address_path("  movies/georgian/ ") == "movies/georgian");
    CHECK(browse::address_path("movies//georgian///") == "movies/georgian");
    CHECK(browse::address_path("~") == "");
    CHECK(browse::address_path("~/") == "");
    CHECK(browse::address_path("~/movies") == "movies");
    CHECK(browse::address_path("~//movies/") == "movies");
    CHECK(browse::address_path("/") == "/");
    CHECK(browse::address_path("//etc//ssh/") == "/etc/ssh");
    CHECK(browse::address_path("") == "");
    CHECK(browse::address_path("~x/y") == "~x/y"); // only "~" alone or "~/" is the home
    CHECK(browse::address_path("Star Wars: A New Hope") == "Star Wars: A New Hope");
}

TEST_CASE("lsjson's answer for a file's path is that one file") {
    const std::vector<Item> file{{"a.avi", "movies/a.avi/a.avi", false, 5, {}}};
    CHECK(browse::maybe_file(file, "movies/a.avi"));
    CHECK(browse::maybe_file(file, "movies/a.avi/")); // a trailing slash still names it
    CHECK_FALSE(browse::maybe_file(file, "movies/b.avi"));
    CHECK_FALSE(browse::maybe_file({{"a.avi", "x", true, {}, {}}}, "movies/a.avi")); // a folder inside
    CHECK_FALSE(browse::maybe_file({{"a.avi", "x", false, 5, {}}, {"b", "y", false, 1, {}}}, "movies/a.avi"));
    CHECK_FALSE(browse::maybe_file({}, "movies/a.avi"));
}

TEST_CASE("a folder listed live") {
    const std::string json = R"([{"Path":"Season 1","Name":"Season 1","Size":-1,"ModTime":"2023-10-15T09:22:33Z","IsDir":true},
        {"Path":".hidden","Name":".hidden","Size":3,"IsDir":false},
        {"Path":"Ep 10.mkv","Name":"Ep 10.mkv","Size":10,"ModTime":"2023-10-15T11:22:33.987654321+02:00","IsDir":false},
        {"Path":"Ep 2.mkv","Name":"Ep 2.mkv","Size":2,"ModTime":"0001-01-01T00:00:00Z","IsDir":false},
        {"Path":"Ep 3.mkv","Name":"Ep 3.mkv","Size":3,"ModTime":"soon","IsDir":false}])";
    const auto items = browse::parse_lsjson(json, "tv/show", true);
    REQUIRE(items.has_value());
    CHECK(*items == std::vector<Item>{{"Season 1", "tv/show/Season 1", true, std::nullopt, std::nullopt, 1697361753},
                                      {"Ep 2.mkv", "tv/show/Ep 2.mkv", false, 2, std::nullopt}, // before 1970: unknown
                                      {"Ep 3.mkv", "tv/show/Ep 3.mkv", false, 3, std::nullopt},
                                      {"Ep 10.mkv", "tv/show/Ep 10.mkv", false, 10, std::nullopt, 1697361753}});
    CHECK(browse::parse_lsjson(json, "", false)->size() == 5); // dot-entries kept when not skipped
    CHECK((*browse::parse_lsjson(json, "", false))[0].path == "Season 1");
    CHECK_FALSE(browse::parse_lsjson("not json", "x", true).has_value());

    engine::Context ctx;
    ctx.config.rclone = "rclone";
    ctx.config.rclone_config = util::path_from_utf8("C:\\grab\\rclone.conf");
    ctx.settings.rclone_remote = "Box";
    ctx.settings.common_flags = {"-P", "--sftp-chunk-size", "255Ki"};
    CHECK(browse::listing_argv(ctx, "tv/show") ==
          std::vector<std::string>{"rclone", "lsjson", "Box:tv/show", "--no-mimetype", "--config",
                                   util::path_to_utf8(*ctx.config.rclone_config), "--sftp-chunk-size", "255Ki"});
}
