#include "browse.hpp"
#include "engine.hpp"
#include "file_list.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace grab;
using browse::Crumb;
using browse::Item;
using file_list::Entry;

namespace {

Entry dir(std::string path) { return Entry{std::move(path), true, std::nullopt}; }
Entry file(std::string path, std::uint64_t size) { return Entry{std::move(path), false, size}; }

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
    const browse::Index index(list_of({dir("movies"), dir("movies/georgian"), file("movies/georgian/b.avi", 50),
                                       file("movies/georgian/a.avi", 100), dir("movies/georgian/2014"), dir("tv"),
                                       file("tv/x.mkv", 10), file("top.txt", 5)}),
                              {""}, 3);
    const auto top = index.top();
    REQUIRE(top.size() == 3);
    CHECK(top[0] == Item{"movies", "movies", true, std::nullopt, 1}); // something below is unknown: no size
    CHECK(top[1] == Item{"tv", "tv", true, 10, 1});                    // all known: its size
    CHECK(top[2] == Item{"top.txt", "top.txt", false, 5, std::nullopt});

    const auto georgian = index.folder("movies/georgian");
    REQUIRE(georgian.has_value());
    CHECK(*georgian == std::vector<Item>{{"2014", "movies/georgian/2014", true, std::nullopt, std::nullopt},
                                         {"a.avi", "movies/georgian/a.avi", false, 100, std::nullopt},
                                         {"b.avi", "movies/georgian/b.avi", false, 50, std::nullopt}});
    CHECK_FALSE(index.folder("movies/georgian/2014").has_value()); // listed live instead
    CHECK_FALSE(index.folder("missing").has_value());
    CHECK_FALSE(index.folder("/etc").has_value()); // outside the roots
    CHECK(index.folder("")->size() == 3);
}

TEST_CASE("several search roots are the server's top folders") {
    const browse::Index index(list_of({dir("/srv/media/a"), file("/srv/media/a/f.mkv", 7), file("/home/x/n.txt", 1)}),
                              {"/srv/media", "/home/x"}, 4);
    CHECK(index.top() == std::vector<Item>{{"/srv/media", "/srv/media", true, 7, 1}, {"/home/x", "/home/x", true, 1, 1}});
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
    CHECK(browse::crumbs({""}, "/elsewhere").empty());
}

TEST_CASE("a folder listed live") {
    const std::string json = R"([{"Path":"Season 1","Name":"Season 1","Size":-1,"IsDir":true},
        {"Path":".hidden","Name":".hidden","Size":3,"IsDir":false},
        {"Path":"Ep 10.mkv","Name":"Ep 10.mkv","Size":10,"IsDir":false},
        {"Path":"Ep 2.mkv","Name":"Ep 2.mkv","Size":2,"IsDir":false}])";
    const auto items = browse::parse_lsjson(json, "tv/show", true);
    REQUIRE(items.has_value());
    CHECK(*items == std::vector<Item>{{"Season 1", "tv/show/Season 1", true, std::nullopt, std::nullopt},
                                      {"Ep 2.mkv", "tv/show/Ep 2.mkv", false, 2, std::nullopt},
                                      {"Ep 10.mkv", "tv/show/Ep 10.mkv", false, 10, std::nullopt}});
    CHECK(browse::parse_lsjson(json, "", false)->size() == 4); // dot-entries kept when not skipped
    CHECK((*browse::parse_lsjson(json, "", false))[0].path == "Season 1");
    CHECK_FALSE(browse::parse_lsjson("not json", "x", true).has_value());

    engine::Context ctx;
    ctx.config.rclone = "rclone";
    ctx.config.rclone_config = util::path_from_utf8("C:\\grab\\rclone.conf");
    ctx.settings.rclone_remote = "Box";
    ctx.settings.common_flags = {"-P", "--sftp-chunk-size", "255Ki"};
    CHECK(browse::listing_argv(ctx, "tv/show") ==
          std::vector<std::string>{"rclone", "lsjson", "Box:tv/show", "--no-mimetype", "--no-modtime", "--config",
                                   util::path_to_utf8(*ctx.config.rclone_config), "--sftp-chunk-size", "255Ki"});
}
