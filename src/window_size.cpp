#include "window_size.hpp"

#include "gui_state.hpp"

#include <algorithm>

namespace grab::window_size {

namespace {

// The largest size each way: max_percent of the work area, but never below the window's minimum
// (min_px: on a screen smaller than that, the whole work area).
int largest(int work, int min_px) { return std::max(work * max_percent / 100, min_px); }

// One way across the screen: where something starts, and its size.
struct Span {
    int start = 0;
    int size = 0;
};

// Where a window that was `was` and grew to `size` starts, one way: where it did, unless it would
// now reach past the work area's far edge (or past where it reached before, if that was further
// out); then it moves back, but not past the work area's near edge (unless it already was).
int kept_in(Span was, int size, Span work) {
    if (size <= was.size) return was.start;
    const int limit = std::max(work.start + work.size, was.start + was.size);
    if (was.start + size <= limit) return was.start;
    return std::max(std::min(was.start, work.start), limit - size);
}

} // namespace

int to_px(int dip, int dpi) { return (dip * dpi + 48) / 96; }

int min_px(int min_dip, int dpi, int work) {
    return work > 0 ? std::min(to_px(min_dip, dpi), work) : to_px(min_dip, dpi);
}

WindowPlacement default_window(const Area& work, int dpi) {
    const int work_w = work.right - work.left;
    const int work_h = work.bottom - work.top;
    const int min_w = min_px(min_width, dpi, work_w);
    const int min_h = min_px(min_height, dpi, work_h);
    const int w = std::clamp(to_px(default_width, dpi), min_w, largest(work_w, min_w));
    const int h = std::clamp(to_px(default_height, dpi), min_h, largest(work_h, min_h));
    return WindowPlacement{work.left + std::max(0, (work_w - w) / 2), work.top + std::max(0, (work_h - h) / 2), w, h,
                           false};
}

WindowPlacement grown_once(const WindowPlacement& saved, int saved_layout, const Area& work, int dpi) {
    if (saved_layout >= layout) return saved;
    const Span across{work.left, work.right - work.left};
    const Span down{work.top, work.bottom - work.top};
    const int w =
        std::max(saved.width, std::min(to_px(default_width, dpi), largest(across.size, min_px(min_width, dpi, across.size))));
    const int h =
        std::max(saved.height, std::min(to_px(default_height, dpi), largest(down.size, min_px(min_height, dpi, down.size))));
    return WindowPlacement{kept_in({saved.x, saved.width}, w, across), kept_in({saved.y, saved.height}, h, down), w, h,
                           saved.maximized};
}

} // namespace grab::window_size
