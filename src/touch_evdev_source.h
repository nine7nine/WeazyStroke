#pragma once
#include "input_event.h"
#include "input_source.h"

#include <cstdint>
#include <string>
#include <vector>

struct libevdev;

namespace es {

class InputSink;

// Reads a touchscreen directly via libevdev (read-only, NOT grabbed) so we can
// see the multitouch contact size (ABS_MT_TOUCH_MAJOR) that libinput's touch API
// hides. That contact size is mapped to a pseudo-pressure (0..1) on each Sample,
// letting the trail get thicker under a fatter/harder touch — the touch analogue
// of the stylus's real pressure. Because it is read-only the compositor still
// gets the touches too (monitor mode); the grab path is a separate later step.
//
// Only the MT slot protocol (type B) is handled. The matching LibinputSource is
// told to skip touch so events are not delivered twice.
class TouchEvdevSource final : public InputSource {
public:
    // screen_w/screen_h define the coordinate space the normalized touch position
    // is scaled into (same space as the rest of the engine). pressure_ref is the
    // TOUCH_MAJOR value that maps to full pressure (<=0 => use the device max).
    TouchEvdevSource(InputSink &sink, int screen_w, int screen_h, double pressure_ref);
    ~TouchEvdevSource() override;

    TouchEvdevSource(const TouchEvdevSource &) = delete;
    TouchEvdevSource &operator=(const TouchEvdevSource &) = delete;

    int fd() const override;
    void dispatch() override;

    const std::string &device_name() const { return name_; }
    bool has_pressure() const { return tmajor_max_ > 0; }
    int contact_max() const { return tmajor_max_; } // device TOUCH_MAJOR range, for calibration

    // Contact-size band mapped to width (floor -> thin, ref -> full; ref<=0 =>
    // device max). Live-tunable so the sensitivity can be calibrated via reload.
    void set_pressure_ref(double r) { pressure_ref_ = r; }
    void set_pressure_floor(double f) { pressure_floor_ = f; }

private:
    // One MT slot's pending state within the current SYN frame.
    struct Slot {
        int tracking_id = -1; // -1 = up
        double x = 0.0;
        double y = 0.0;
        double pressure = -1.0;
        int peak_major = 0;    // largest raw TOUCH_MAJOR this contact (calibration)
        int motions = 0;       // motion events emitted this contact (debug)
        bool down = false;     // currently in contact (reported to the sink)
        bool moved = false;    // x/y changed this frame
        bool changed = false;  // tracking_id changed this frame
    };

    bool open_touchscreen();             // finds + opens a direct touchscreen
    void read_events();
    void flush_frame(uint32_t time_ms);
    double scale_x(int v) const;
    double scale_y(int v) const;
    double scale_pressure(int v) const;

    InputSink &sink_;
    int screen_w_;
    int screen_h_;
    double pressure_ref_;
    double pressure_floor_ = 150.0;

    int fd_ = -1;
    libevdev *evdev_ = nullptr;
    std::string name_;

    int xmin_ = 0, xmax_ = 0;
    int ymin_ = 0, ymax_ = 0;
    int tmajor_max_ = 0; // device's reported TOUCH_MAJOR max (used if no ref set)

    std::vector<Slot> slots_;
    int cur_slot_ = 0;
};

} // namespace es
