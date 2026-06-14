#pragma once
#include "input_event.h"
#include <string>

namespace es {

// Consumer of normalized input events. The gesture state machine implements
// this; an InputSource feeds it. This is the seam that decouples gesture logic
// from the platform's input mechanism (libinput today, raw evdev later).
class InputSink {
public:
    virtual ~InputSink() = default;

    virtual void on_button(Button button, bool pressed, Sample at) = 0;
    virtual void on_motion(Sample at, double dx, double dy) = 0;
    virtual void on_scroll(double dx, double dy, Sample at) = 0;

    // Multitouch. `slot` identifies the finger (libinput's MT slot). Default:
    // ignore, so sinks that only care about pointer/pen need not implement them.
    virtual void on_touch_down(int slot, Sample at) { (void)slot, (void)at; }
    virtual void on_touch_motion(int slot, Sample at) { (void)slot, (void)at; }
    virtual void on_touch_up(int slot, Sample at) { (void)slot, (void)at; }

    // Current keyboard modifier state (Mod bits), updated as modifiers change.
    // Used to gate the trigger in mouse mode (e.g. Super+click). Default: ignore.
    virtual void on_modifiers(unsigned mask) { (void)mask; }

    virtual void on_device_added(const std::string &name) { (void)name; }
    virtual void on_device_removed(const std::string &name) { (void)name; }
};

} // namespace es
