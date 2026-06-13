#pragma once
#include "input_injector.h"
#include <cstdint>

namespace es {

// Injects input via a virtual /dev/uinput device. Requires read-write access to
// /dev/uinput (input group + a udev rule, or running as the privileged daemon).
//
// Exposes a relative pointer + buttons + wheel + full key range. Absolute
// warping is deliberately not implemented yet (see InputInjector).
class UinputInjector final : public InputInjector {
public:
    // The created virtual device's name. The evdev grabber uses this to avoid
    // grabbing our own injected-output device.
    static constexpr const char *kDeviceName = "easystroke-wayland virtual input";

    // Opens /dev/uinput and creates the device. Throws std::system_error on
    // failure (e.g. permission denied, module not loaded).
    UinputInjector();
    ~UinputInjector() override;

    UinputInjector(const UinputInjector &) = delete;
    UinputInjector &operator=(const UinputInjector &) = delete;

    void move_relative(double dx, double dy) override;
    void button(Button logical, bool pressed) override;
    void scroll(int dx, int dy) override;
    void key(uint16_t evdev_keycode, bool pressed) override;
    void flush() override;

private:
    void emit(uint16_t type, uint16_t code, int32_t value);

    int fd_ = -1;
    // Sub-pixel relative motion is accumulated; only whole units are emitted.
    double accum_x_ = 0.0;
    double accum_y_ = 0.0;
};

} // namespace es
