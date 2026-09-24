#include "gui_state.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <map>
#include <string>
#include <vector>

using namespace grab;

TEST_CASE("gui state round-trips through JSON and a file") {
    GuiState s;
    s.window = WindowPlacement{-8, 20, 1280, 800, true};
    s.last_remote = "hetzner";
    s.mode = "folder";
    s.destinations = {{"hetzner", "E:\\TV"}, {"arch_guest", "D:\\Box \"quoted\""}};
    s.parallel = 6;
    s.server_limits = {{"arch_guest", 7}};

    CHECK(parse_gui_state(gui_state_to_json(s)) == s);

    const auto file = std::filesystem::temp_directory_path() / "grab_test_gui" / "gui.json";
    std::filesystem::remove_all(file.parent_path());
    CHECK(save_gui_state(file, s)); // creates the folder
    CHECK(load_gui_state(file) == s);
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
    CHECK(partial.mode == "file");
    CHECK(partial.last_remote == "box");
    CHECK(partial.destinations == std::map<std::string, std::string>{{"box", "C:\\x"}});

    CHECK(load_gui_state("Z:\\definitely\\missing\\gui.json") == GuiState{});

    // Parallel downloads stay within 1..8; nonsense limits are dropped.
    CHECK(parse_gui_state(R"({"parallel":0})").parallel == 1);
    CHECK(parse_gui_state(R"({"parallel":99})").parallel == 8);
    CHECK(parse_gui_state(R"({})").parallel == 4);
    CHECK(parse_gui_state(R"({"serverLimits":{"a":3,"b":0,"c":"x"}})").server_limits ==
          std::map<std::string, int>{{"a", 3}});
}

TEST_CASE("the download queue round-trips and skips incomplete entries") {
    std::vector<SavedDownload> q{{"hetzner", "file", "/home/x/a.mkv", "a.mkv", "E:\\TV", 123456789012ULL},
                                 {"arch_guest", "folder", "/home/music/Album", "Album", "D:\\Music", 0}};
    CHECK(parse_queue(queue_to_json(q)) == q);
    CHECK(parse_queue("nope").empty());
    const auto partial = parse_queue(R"([{"remote":"a","path":"/x","dest":"C:\\d"},{"remote":"b"},7])");
    REQUIRE(partial.size() == 1);
    CHECK(partial[0].name == "/x");
    CHECK(partial[0].mode == "file");
}
