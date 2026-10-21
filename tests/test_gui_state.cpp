#include "gui_state.hpp"
#include "json.hpp"
#include "util.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

using namespace grab;

TEST_CASE("gui state round-trips through JSON and a file") {
    GuiState s;
    s.window = WindowPlacement{-8, 20, 1280, 800, true};
    s.last_remote = "hetzner";
    s.sort = SortOrder{"modified", "desc"};
    s.destinations = {{"hetzner", "E:\\TV"}, {"arch_guest", "D:\\Box \"quoted\""}};
    s.parallel = 6;
    s.server_limits = {{"arch_guest", 7}};
    s.timings = {{"lsjson:Box.Guest", 2300}, {"update", 900}};
    s.limit_on = true;
    s.limit_mibps = 2.5;
    s.speed_schedule = {{0x1F, 18 * 60, 0, 5}, {0x60, 22 * 60, 6 * 60, 2.5}};
    s.limit_change = schedule::Override{{false, 5}, 5, 1760003600};
    s.verify_downloads = true;
    s.update_check = false;
    s.last_update_check = 1760000000;
    s.latest_version = "0.7.0";
    s.latest_page = "https://github.com/Giorgi-Apriashvili/grab/releases/tag/v0.7.0";

    CHECK(parse_gui_state(gui_state_to_json(s)) == s);
    CHECK(parse_gui_state("{}").update_check); // on unless switched off
    CHECK_FALSE(parse_gui_state("{}").verify_downloads); // off unless switched on

    const auto file = std::filesystem::temp_directory_path() / "grab_test_gui" / "gui.json";
    std::filesystem::remove_all(file.parent_path());
    CHECK(save_gui_state(file, s)); // creates the folder
    const auto back = read_gui_state(file);
    REQUIRE(back.has_value());
    CHECK(*back == std::optional<GuiState>(s));
    std::filesystem::remove_all(file.parent_path());
}

TEST_CASE("broken or partial gui state falls back to defaults field by field") {
    CHECK(parse_gui_state("") == GuiState{});
    CHECK(parse_gui_state("not json") == GuiState{});
    CHECK(parse_gui_state("[1,2]") == GuiState{});

    const auto partial = parse_gui_state(
        R"({"window":{"x":1,"y":2,"width":0,"height":5},"mode":"sideways","lastRemote":"box",)"
        R"("destinations":{"box":"C:\\x","bad":7,"empty":""}})");
    CHECK_FALSE(partial.window.has_value()); // zero width is not a usable placement
    CHECK(partial.last_remote == "box");
    CHECK(partial.destinations == std::map<std::string, std::string>{{"box", "C:\\x"}});

    const auto missing = read_gui_state(std::filesystem::temp_directory_path() / "grab-test-missing" / "gui.json");
    CHECK((missing.has_value() && !missing->has_value())); // none: the defaults, nothing to say

    // Parallel downloads stay within 1..8; nonsense limits are dropped.
    CHECK(parse_gui_state(R"({"limitMiBps":0})").limit_mibps == 5.0); // out of range: default
    CHECK_FALSE(parse_gui_state(R"({"limitOn":"yes"})").limit_on);
    CHECK(parse_gui_state(R"({"parallel":0})").parallel == 1);
    CHECK(parse_gui_state(R"({"parallel":99})").parallel == 8);
    CHECK(parse_gui_state(R"({})").parallel == 4);
    CHECK(parse_gui_state(R"({"serverLimits":{"a":3,"b":0,"c":"x"}})").server_limits ==
          std::map<std::string, int>{{"a", 3}});
    CHECK(parse_gui_state(R"({"timings":{"a":1500,"b":0,"c":"x","d":99999999}})").timings ==
          std::map<std::string, int>{{"a", 1500}});

    // The speed schedule keeps its valid rules; a broken change of the limit is dropped.
    CHECK(parse_gui_state(R"({"speedSchedule":[{"days":["sat"],"from":"10:00","to":"12:00","mibps":8},{"days":[]}]})")
              .speed_schedule == std::vector<schedule::Rule>{{0x20, 10 * 60, 12 * 60, 8}});
    CHECK(parse_gui_state(R"({"speedSchedule":"weekends"})").speed_schedule.empty());
    CHECK(parse_gui_state(R"({"limitChange":{"on":false,"mibps":5,"scheduled":5,"until":1760003600}})").limit_change);
    CHECK_FALSE(parse_gui_state(R"({"limitChange":{"on":false,"mibps":5,"scheduled":0,"until":1760003600}})").limit_change);
    CHECK_FALSE(parse_gui_state(R"({"limitChange":{"on":false,"mibps":5,"scheduled":5}})").limit_change);
    CHECK_FALSE(parse_gui_state(R"({"limitChange":{"on":false,"mibps":5,"scheduled":5,"until":1e300}})").limit_change);
}

TEST_CASE("Browse's folders are not kept: a start opens each server at its top") {
    // gui.json from 0.10 and 0.11 held the folder last open per server; it is dropped on the next save.
    const auto s = parse_gui_state(R"({"lastRemote":"Main","browsePaths":{"Box.Guest":"movies","*":"Main"}})");
    CHECK(s.last_remote == "Main");
    CHECK(gui_state_to_json(s).find("browsePaths") == std::string::npos);
}

TEST_CASE("the window has one view: an older gui.json's mode is dropped") {
    // Files, Folders or Browse, up to 0.13; the window is Browse only since.
    for (const char* mode : {"file", "folder", "browse"}) {
        const auto s = parse_gui_state(std::string(R"({"lastRemote":"Box","mode":")") + mode + R"("})");
        CHECK(s.last_remote == "Box");
        CHECK(gui_state_to_json(s).find("\"mode\"") == std::string::npos);
    }
}

TEST_CASE("the results' sort is kept when valid, and absent for the default order") {
    CHECK_FALSE(parse_gui_state("{}").sort.has_value());
    CHECK(gui_state_to_json(GuiState{}).find("\"sort\"") == std::string::npos);
    for (const char* key : {"name", "size", "modified"}) {
        for (const char* dir : {"asc", "desc"}) {
            GuiState s;
            s.sort = SortOrder{key, dir};
            CHECK(parse_gui_state(gui_state_to_json(s)).sort == s.sort);
        }
    }
    CHECK(parse_gui_state(R"({"sort":{"key":"size","dir":"asc"}})").sort == SortOrder{"size", "asc"});
    CHECK_FALSE(parse_gui_state(R"({"sort":{"key":"date","dir":"desc"}})").sort.has_value());
    CHECK_FALSE(parse_gui_state(R"({"sort":{"key":"size","dir":"up"}})").sort.has_value());
    CHECK_FALSE(parse_gui_state(R"({"sort":{"key":"size"}})").sort.has_value());
    CHECK_FALSE(parse_gui_state(R"({"sort":"name"})").sort.has_value());
    CHECK_FALSE(parse_gui_state(R"({"sort":null})").sort.has_value());

    // As the page sends it in "prefs".
    const auto msg = json::parse(R"({"type":"prefs","sort":{"key":"modified","dir":"desc"}})");
    REQUIRE(msg.has_value());
    CHECK(sort_from_json(*msg->find("sort")) == SortOrder{"modified", "desc"});
    CHECK_FALSE(sort_from_json(json::Value{}).has_value()); // null: the default order
    CHECK(json::stringify(sort_to_json(SortOrder{"name", "asc"})) == R"({"key":"name","dir":"asc"})");
}

TEST_CASE("a Size sort keeps the sort before it, which orders the folders") {
    const auto then = R"({"sort":{"key":"size","dir":"desc","then":{"key":"modified","dir":"asc"}}})";
    CHECK(parse_gui_state(then).sort == SortOrder{"size", "desc", "modified", "asc"});
    GuiState s;
    s.sort = SortOrder{"size", "asc", "name", "desc"};
    CHECK(parse_gui_state(gui_state_to_json(s)).sort == s.sort);
    CHECK(json::stringify(sort_to_json(SortOrder{"size", "desc", "name", "asc"})) ==
          R"({"key":"size","dir":"desc","then":{"key":"name","dir":"asc"}})");
    // A `then` that isn't a Name or Modified sort is dropped; the Size sort stays.
    CHECK(parse_gui_state(R"({"sort":{"key":"size","dir":"desc","then":{"key":"size","dir":"asc"}}})").sort ==
          SortOrder{"size", "desc"});
    CHECK(parse_gui_state(R"({"sort":{"key":"size","dir":"desc","then":{"key":"name","dir":"up"}}})").sort ==
          SortOrder{"size", "desc"});
    CHECK(parse_gui_state(R"({"sort":{"key":"size","dir":"desc","then":"name"}})").sort == SortOrder{"size", "desc"});
    CHECK(parse_gui_state(R"({"sort":{"key":"size","dir":"desc","then":null}})").sort == SortOrder{"size", "desc"});
    // Only a Size sort has one.
    CHECK(parse_gui_state(R"({"sort":{"key":"name","dir":"asc","then":{"key":"modified","dir":"asc"}}})").sort ==
          SortOrder{"name", "asc"});
}

TEST_CASE("queue order: move an item before another, or to the end") {
    std::vector<int> ids{1, 2, 3, 4};
    CHECK(move_before(ids, 4, 1)); // to the top
    CHECK(ids == std::vector<int>{4, 1, 2, 3});
    CHECK(move_before(ids, 4, 0)); // to the end
    CHECK(ids == std::vector<int>{1, 2, 3, 4});
    CHECK(move_before(ids, 1, 3)); // down, before 3
    CHECK(ids == std::vector<int>{2, 1, 3, 4});
    CHECK_FALSE(move_before(ids, 1, 3)); // already there
    CHECK_FALSE(move_before(ids, 2, 2));
    CHECK_FALSE(move_before(ids, 9, 1));
    CHECK_FALSE(move_before(ids, 1, 9));
    CHECK(ids == std::vector<int>{2, 1, 3, 4});
}

TEST_CASE("recent searches are sealed on disk and come back") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-recent";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto file = dir / "recent";
    CHECK(load_recent(file).items.empty()); // none yet
    auto items = json::parse(R"([{"q":"lioness s03","remote":"Main","path":"downloads/tv","exact":false,"kinds":["video"],"min":1073741824},)"
                             R"({"q":"winrar","remote":"*"},"not an entry",7])");
    REQUIRE(items.has_value());
    REQUIRE(save_recent(file, *items));
    const auto text = util::read_file(file);
    REQUIRE(text.has_value());
    CHECK(text->find("lioness") == std::string::npos); // sealed: not readable as text
    const auto back = load_recent(file);
    REQUIRE(back.items.size() == 2); // the objects only
    CHECK(back.items[0].string_of("q") == "lioness s03");
    CHECK(back.items[1].string_of("remote") == "*");

    json::Value many = json::Value::make_array();
    for (int i = 0; i < 30; ++i) {
        json::Value e = json::Value::make_object();
        e.set("q", json::Value::make_string("search " + std::to_string(i)));
        many.push(std::move(e));
    }
    REQUIRE(save_recent(file, many));
    CHECK(load_recent(file).items.size() == recent_max);
    CHECK(load_recent(file).items[0].string_of("q") == "search 0"); // the newest are kept

    REQUIRE(save_recent(file, json::Value::make_array())); // Clear history
    CHECK_FALSE(std::filesystem::exists(file));
    REQUIRE(util::write_file_atomic(file, R"([{"q":"plain"}])")); // not sealed: not used...
    CHECK(load_recent(file).items.empty());
    // ...and set aside, so the next search's save doesn't replace it (G215).
    CHECK_FALSE(std::filesystem::exists(file));
    bool kept = false;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (util::path_to_utf8(e.path().filename()).starts_with("recent.bad-")) {
            kept = util::read_file(e.path()).value_or("") == R"([{"q":"plain"}])";
        }
    }
    CHECK(kept);
    std::filesystem::remove_all(dir);
}

TEST_CASE("the download queue round-trips and skips incomplete entries") {
    std::vector<SavedDownload> q{{"hetzner", "file", "/home/x/a.mkv", "a.mkv", "E:\\TV", 123456789012ULL, {}, "high"},
                                 {"arch_guest", "folder", "/home/music/Album", "Album", "D:\\Music", 0, {}, "low"}};
    CHECK(parse_queue(queue_to_json(q)) == q);
    q[0].resume = true; // saved for an update's restart
    CHECK(parse_queue(queue_to_json(q)) == q);
    CHECK_FALSE(parse_queue(queue_to_json(q))[1].resume);
    q[1].space_ok = true; // "Download anyway", or Resume after the free-space check paused it
    CHECK(parse_queue(queue_to_json(q)) == q);
    CHECK_FALSE(parse_queue(queue_to_json(q))[0].space_ok);
    q[0].existing = "both"; // the answer to "already there", and the file's second-copy name
    q[0].local_name = "a (2).mkv";
    CHECK(parse_queue(queue_to_json(q)) == q);
    CHECK(parse_queue(R"([{"remote":"a","path":"/x","dest":"C:\\d","existing":"maybe"}])")[0].existing.empty());
    CHECK(parse_queue(R"([{"remote":"a","path":"/x","dest":"C:\\d","priority":"urgent"}])")[0].priority == "normal");
    CHECK(parse_queue("nope").empty());
    const auto partial = parse_queue(R"([{"remote":"a","path":"/x","dest":"C:\\d"},{"remote":"b"},7])");
    REQUIRE(partial.size() == 1);
    CHECK(partial[0].name == "/x");
    CHECK(partial[0].mode == "file");
}

TEST_CASE("saved settings and a saved queue that can't be read are told apart from none (G212, G213)") {
    const auto dir = std::filesystem::temp_directory_path() / "grab-test-damaged-state";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const auto gui = dir / "gui.json";
    const auto queue = dir / "queue.json";
    const auto none_gui = read_gui_state(gui);
    CHECK((none_gui.has_value() && !none_gui->has_value()));
    const auto none_queue = read_queue(queue);
    CHECK((none_queue.has_value() && !none_queue->has_value()));

    // Cut off, or not what grab saves: errors, so the window sets them aside instead of saving
    // the defaults or an empty queue over them.
    REQUIRE(util::write_file_atomic(gui, R"({"verifyDownloads":true,"updateCh)"));
    CHECK_FALSE(read_gui_state(gui).has_value());
    REQUIRE(util::write_file_atomic(gui, "[1,2]"));
    CHECK_FALSE(read_gui_state(gui).has_value());
    REQUIRE(util::write_file_atomic(gui, R"({"verifyDownloads":true})"));
    const auto good = read_gui_state(gui);
    REQUIRE(good.has_value());
    CHECK(good->value_or(GuiState{}).verify_downloads);

    REQUIRE(util::write_file_atomic(queue, R"([{"remote":"A","path":"/x","dest":"C:\\d"},{"remote":"B","pa)"));
    CHECK_FALSE(read_queue(queue).has_value());
    REQUIRE(util::write_file_atomic(queue, "{}"));
    CHECK_FALSE(read_queue(queue).has_value());
    REQUIRE(util::write_file_atomic(queue, R"([{"remote":"A","path":"/x","dest":"C:\\d"}])"));
    const auto one = read_queue(queue);
    REQUIRE(one.has_value());
    CHECK(one->value_or(std::vector<SavedDownload>{}).size() == 1);
    std::filesystem::remove_all(dir);
}