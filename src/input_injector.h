#pragma once
#include "input_event.h"
#include <cstdint>

namespace es {

// Synthesizes input back into the system — the Wayland replacement for X11's
// XTest. The only implementation today is UinputInjector (/dev/uinput).
class InputInjector {
public:
    virtual ~InputInjector() = default;

    virtual void move_relative(double dx, double dy) = 0;
    virtual void button(Button logical, bool pressed) = 0;
    virtual void scroll(int dx, int dy) = 0;          // in wheel detents
    virtual void key(uint16_t evdev_keycode, bool pressed) = 0;
    virtual void flush() = 0;                          // emit SYN_REPORT

    // Convenience helpers built on the primitives above.
    void click(Button logical) {
        button(logical, true);
        button(logical, false);
        flush();
    }
    void tap(uint16_t evdev_keycode) {
        key(evdev_keycode, true);
        key(evdev_keycode, false);
        flush();
    }

    // NOTE: absolute pointer warping (X11 move-pointer-back) is intentionally
    // absent from v0. It needs a separate ABS uinput device; it will be added
    // when the gesture state machine that uses "move back" is ported.
};

} // namespace es
