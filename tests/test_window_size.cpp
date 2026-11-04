#include "gui_state.hpp"
#include "window_size.hpp"

#include <doctest/doctest.h>

using namespace grab;
using window_size::Area;
using window_size::default_window;
using window_size::grown_once;

TEST_CASE("the window starts at 1280x860 DIP, at most 92% of the work area, centred") {
    // 1920x1080 at 100% with the taskbar.
    CHECK(default_window(Area{0, 0, 1920, 1040}, 96) == WindowPlacement{320, 90, 1280, 860, false});
    // 150%: the same window in pixels.
    CHECK(default_window(Area{0, 0, 2880, 1560}, 144) == WindowPlacement{480, 135, 1920, 1290, false});
    // 125% on a 1920x1080 screen (1536x824 DIP): 1600x1075 px is too tall, 92% of 1030 px is used.
    CHECK(default_window(Area{0, 0, 1920, 1030}, 120) == WindowPlacement{160, 41, 1600, 947, false});
    // A work area of 1536x824 pixels at 120 DPI: both ways clamped to 92%.
    CHECK(default_window(Area{0, 0, 1536, 824}, 120) == WindowPlacement{61, 33, 1413, 758, false});
    // A secondary monitor left of the primary: inside its own work area.
    CHECK(default_window(Area{-1920, 0, 0, 1040}, 96) == WindowPlacement{-1600, 90, 1280, 860, false});
    // A screen lower than the window's minimum (760x600 DIP) gets its whole height.
    CHECK(default_window(Area{0, 0, 800, 560}, 96) == WindowPlacement{20, 0, 760, 560, false});
    // 1366x768 at 125%: the minimum would be 950x750 px, the work area is 708 px high.
    CHECK(default_window(Area{0, 0, 1366, 708}, 120) == WindowPlacement{55, 0, 1256, 708, false});
    // No work area known (the monitor's info failed): the minimum.
    CHECK(default_window(Area{}, 96) == WindowPlacement{0, 0, 760, 600, false});
    CHECK(window_size::to_px(1280, 96) == 1280);
    CHECK(window_size::to_px(860, 120) == 1075);
    CHECK(window_size::to_px(760, 144) == 1140);
}

TEST_CASE("the window's minimum fits the work area") {
    CHECK(window_size::min_px(600, 96, 1040) == 600);
    CHECK(window_size::min_px(600, 120, 708) == 708);
    CHECK(window_size::min_px(760, 120, 1366) == 950);
    // Unknown (0) or nonsense sizes leave the minimum alone.
    CHECK(window_size::min_px(600, 120, 0) == 750);
    CHECK(window_size::min_px(600, 120, -5) == 750);
}

TEST_CASE("a window saved smaller by an earlier grab grows once, each way, never shrinking") {
    const Area work{0, 0, 1920, 1040};
    // grab 0.21's default grows to the new one, where it was.
    CHECK(grown_once({100, 50, 1100, 720, false}, 0, work, 96) == WindowPlacement{100, 50, 1280, 860, false});
    // Larger both ways: kept.
    CHECK(grown_once({100, 50, 1400, 950, false}, 0, work, 96) == WindowPlacement{100, 50, 1400, 950, false});
    // Wider but lower: only the height grows.
    CHECK(grown_once({100, 50, 1400, 700, false}, 0, work, 96) == WindowPlacement{100, 50, 1400, 860, false});
    // Maximized stays maximized; its restored size grows.
    CHECK(grown_once({100, 50, 1100, 720, true}, 0, work, 96) == WindowPlacement{100, 50, 1280, 860, true});
    // Once layout 2 is saved, never again.
    CHECK(grown_once({100, 50, 1100, 720, false}, window_size::layout, work, 96) ==
          WindowPlacement{100, 50, 1100, 720, false});
    CHECK(grown_once({100, 50, 1100, 720, false}, 3, work, 96) == WindowPlacement{100, 50, 1100, 720, false});
    // At the saved monitor's DPI: 1100x720 DIP at 150% is 1650x1080 px.
    CHECK(grown_once({0, 0, 1650, 1080, false}, 0, Area{0, 0, 2880, 1560}, 144) ==
          WindowPlacement{0, 0, 1920, 1290, false});
}

TEST_CASE("a window that grows once stays on its work area") {
    const Area work{0, 0, 1920, 1040};
    // Near the right and bottom edges: moved back just enough.
    CHECK(grown_once({700, 300, 1100, 720, false}, 0, work, 96) == WindowPlacement{640, 180, 1280, 860, false});
    // Already past the right edge: it reaches no further than before.
    CHECK(grown_once({900, 0, 1100, 720, false}, 0, work, 96) == WindowPlacement{720, 0, 1280, 860, false});
    // Partly off the left edge, with room on the right: left where it is.
    CHECK(grown_once({-10, 0, 1100, 720, false}, 0, work, 96) == WindowPlacement{-10, 0, 1280, 860, false});
    // A small work area: at most 92% of it, never past its top-left corner.
    CHECK(grown_once({0, 0, 1100, 720, false}, 0, Area{0, 0, 1366, 728}, 96) ==
          WindowPlacement{0, 0, 1256, 720, false});
    CHECK(grown_once({200, 100, 1000, 600, false}, 0, Area{0, 0, 1366, 728}, 96) ==
          WindowPlacement{110, 59, 1256, 669, false});
    // A work area lower than the minimum: the window grows to its height, no further.
    CHECK(grown_once({0, 0, 1100, 500, false}, 0, Area{0, 0, 1366, 708}, 120) ==
          WindowPlacement{0, 0, 1256, 708, false});
    // A secondary monitor above the primary.
    CHECK(grown_once({50, -1000, 1100, 720, false}, 0, Area{0, -1080, 1920, 0}, 96) ==
          WindowPlacement{50, -1000, 1280, 860, false});
    CHECK(grown_once({50, -800, 1100, 720, false}, 0, Area{0, -1080, 1920, 0}, 96) ==
          WindowPlacement{50, -860, 1280, 860, false});
}
