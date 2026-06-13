#pragma once
#include "input_event.h"
#include "input_source.h"
#include "trigger_gate.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct libevdev;

namespace es {

class InputSink;
class InputInjector;

// Grabs mouse-like devices with EVIOCGRAB and forwards their events through the
// injector, so it can *suppress* the trigger button: a gesture-drag no longer
// also triggers the button's normal action, while a plain click is replayed.
//
// This is the Wayland equivalent of X11's passive XIGrabButton, and the reason
// the input engine eventually runs as a privileged daemon. Safety: only mice
// are grabbed (the keyboard stays free, so Ctrl-C always works), and the kernel
// releases every grab when the process exits.
class EvdevSource final : public InputSource {
public:
    EvdevSource(InputSink &sink, InputInjector &injector, Button trigger, int screen_w,
                int screen_h);
    ~EvdevSource() override;

    EvdevSource(const EvdevSource &) = delete;
    EvdevSource &operator=(const EvdevSource &) = delete;

    int fd() const override; // epoll fd aggregating all grabbed devices
    void dispatch() override;

    size_t device_count() const { return devices_.size(); }

private:
    struct Device {
        int fd = -1;
        libevdev *evdev = nullptr;
        std::string name;
    };

    void scan_and_grab();
    void try_add(const std::string &path);
    bool looks_like_mouse(libevdev *ev) const;
    void read_device(Device &d);
    void process_button(uint16_t code, int value, uint32_t time_ms);
    void flush_frame(uint32_t time_ms);
    void clamp_position();

    InputSink &sink_;
    InputInjector &injector_;
    Button trigger_;
    uint16_t trigger_evdev_;
    int screen_w_;
    int screen_h_;
    int epoll_fd_ = -1;
    std::vector<std::unique_ptr<Device>> devices_;

    Point pos_;
    Point press_origin_;
    TriggerGate gate_;

    // Per-SYN-frame accumulators for relative motion and wheel.
    double frame_dx_ = 0.0;
    double frame_dy_ = 0.0;
    int frame_wheel_ = 0;
    int frame_hwheel_ = 0;
    bool frame_motion_ = false;
    bool frame_wheel_evt_ = false;
};

} // namespace es
