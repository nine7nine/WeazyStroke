#include "button_map.h"
#include <linux/input-event-codes.h>

namespace es {

Button evdev_to_logical(uint16_t code) {
    switch (code) {
    case BTN_LEFT:    return 1;
    case BTN_MIDDLE:  return 2;
    case BTN_RIGHT:   return 3;
    case BTN_SIDE:    return 8; // "back" thumb button
    case BTN_BACK:    return 8;
    case BTN_EXTRA:   return 9; // "forward" thumb button
    case BTN_FORWARD: return 9;
    default:          return 0;
    }
}

uint16_t logical_to_evdev(Button logical) {
    switch (logical) {
    case 1: return BTN_LEFT;
    case 2: return BTN_MIDDLE;
    case 3: return BTN_RIGHT;
    case 8: return BTN_SIDE;
    case 9: return BTN_EXTRA;
    default: return 0; // wheel (4-7) or unknown
    }
}

bool is_wheel_button(Button logical) {
    return logical >= 4 && logical <= 7;
}

} // namespace es
