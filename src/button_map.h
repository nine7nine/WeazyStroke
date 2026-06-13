#pragma once
#include "input_event.h"
#include <cstdint>

namespace es {

// Logical numbers for tablet-tool ("pen") buttons. Placed above X's 1-9 range so
// they never collide with mouse buttons, and so a pen event can drive the same
// trigger machinery as a mouse button. Used as gesture triggers for stylus input.
constexpr Button kPenTip = 10;     // BTN_TOUCH  — pen tip in contact with the surface
constexpr Button kPenButton = 11;  // BTN_STYLUS — primary barrel/side button
constexpr Button kPenButton2 = 12; // BTN_STYLUS2 — secondary barrel button (if present)

// Translation between Linux evdev button codes (BTN_*) and easystroke's logical
// X-style button numbers. See input_event.h for the numbering.

// Returns the logical button for an evdev code, or 0 if unmapped.
Button evdev_to_logical(uint16_t evdev_code);

// Returns the evdev BTN_* code for a logical button, or 0 for wheel/unknown
// buttons (which are injected as scroll events, not key events).
uint16_t logical_to_evdev(Button logical);

// True for logical buttons 4..7, which represent wheel directions rather than
// physical buttons.
bool is_wheel_button(Button logical);

} // namespace es
