#pragma once
#include "input_event.h"
#include <cstdint>

namespace es {

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
