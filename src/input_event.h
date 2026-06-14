#pragma once
#include <cstdint>

namespace es {

// Logical pointer-button numbering, kept identical to the X11 easystroke
// convention so the recycled action database and gesture logic stay compatible:
//   1 = left, 2 = middle, 3 = right,
//   4/5 = wheel up/down, 6/7 = wheel left/right,
//   8 = back, 9 = forward
using Button = unsigned;

// Trigger modifier bits, for mouse-mode activation (e.g. Super+click). Shared by
// the config GUI, the input source (which tracks keyboard state) and the
// recognizer (which gates the trigger on them).
enum : unsigned { kModCtrl = 1u, kModAlt = 2u, kModShift = 4u, kModSuper = 8u };

struct Point {
    double x = 0.0;
    double y = 0.0;
};

// A timestamped pointer sample — the "triple" (x, y, t) the recognition core
// consumes. Time is in milliseconds, matching libinput's event timestamps.
struct Sample {
    double x = 0.0;
    double y = 0.0;
    uint32_t time_ms = 0;
    // Pen pressure, 0..1. Negative means "no pressure data" (mouse / touch), so
    // only stylus strokes drive pressure-sensitive trail width.
    double pressure = -1.0;
};

} // namespace es
