#pragma once

// grab-gui's window size (DOWNLOADS-UI.md 4.2): the default at the first start, and the one-time
// growth of a window an earlier grab saved smaller. Pure, in physical pixels, so it is
// unit-tested; main_gui.cpp passes in the monitor's work area and DPI.

#include "gui_state.hpp"

namespace grab::window_size {

// The default and the smallest window, in DIP (pixels at 96 DPI). The minimum leaves room for
// Browse's and Downloads' own minimums (the page's splitter) without a banner.
inline constexpr int default_width = 1280;
inline constexpr int default_height = 860;
inline constexpr int min_width = 760;
inline constexpr int min_height = 600;
// A window never starts or grows past this share of its monitor's work area, each way.
inline constexpr int max_percent = 92;
// gui.json's "layout" once the window is placed by these rules (grab 0.22); a file without it
// came from an earlier grab, whose default was 1100x720.
inline constexpr int layout = 2;

// A monitor's work area (without the taskbar), in screen pixels.
struct Area {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

// `dip` at `dpi` in pixels, rounded as MulDiv rounds.
[[nodiscard]] int to_px(int dip, int dpi);

// The window's minimum `min_dip` at `dpi` in pixels, but at most `work` (the work area's size that
// way, when known: above 0), so a window on a small screen still fits it, maximized too.
[[nodiscard]] int min_px(int min_dip, int dpi, int work);

// The window at its first start: the default size, at most max_percent of the work area each
// way (but never below the minimum, itself capped at the work area), centred in it.
[[nodiscard]] WindowPlacement default_window(const Area& work, int dpi);

// A saved window as it opens, `saved_layout` being gui.json's. One saved before `layout` 2 that is
// smaller than the default grows once: each way on its own to the default, at `dpi` (its
// monitor's), at most max_percent of the work area and never smaller than it was. Its top-left
// corner stays unless the bigger window would reach past the work area's right or bottom edge;
// then it moves back as far as needed, but not past the left or top edge. Maximized stays
// maximized. From layout 2 on it is returned as saved.
[[nodiscard]] WindowPlacement grown_once(const WindowPlacement& saved, int saved_layout, const Area& work, int dpi);

} // namespace grab::window_size
