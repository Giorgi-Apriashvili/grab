#include "gui_state.hpp"

#include <doctest/doctest.h>

#include <filesystem>

using namespace grab;

TEST_CASE("gui state round-trips through JSON and a file") {
    GuiState s;
    s.window = WindowPlacement{-8, 20, 1280, 800, true};
    s.last_remote = "hetzner";
    s.mode = "folder";
    s.destinations = {{"hetzner", "E:\\TV"}, {"arch_guest", "D:\\Box \"quoted\""}};

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
}
