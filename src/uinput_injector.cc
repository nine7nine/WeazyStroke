#include "uinput_injector.h"
#include "button_map.h"

#include <fcntl.h>
#include <linux/uinput.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <system_error>

namespace es {

namespace {

void check(int rc, const char *what) {
    if (rc < 0)
        throw std::system_error(errno, std::generic_category(), what);
}

} // namespace

UinputInjector::UinputInjector() {
    fd_ = ::open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    check(fd_, "open /dev/uinput");

    // Event types we will emit.
    check(ioctl(fd_, UI_SET_EVBIT, EV_KEY), "UI_SET_EVBIT EV_KEY");
    check(ioctl(fd_, UI_SET_EVBIT, EV_REL), "UI_SET_EVBIT EV_REL");
    check(ioctl(fd_, UI_SET_EVBIT, EV_SYN), "UI_SET_EVBIT EV_SYN");

    // Relative axes: motion + both wheels.
    for (int axis : {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL})
        check(ioctl(fd_, UI_SET_RELBIT, axis), "UI_SET_RELBIT");

    // Enable the full key/button range. BTN_* codes live inside this range, so
    // this covers pointer buttons as well as keyboard keys.
    for (int code = 1; code < KEY_MAX; ++code)
        check(ioctl(fd_, UI_SET_KEYBIT, code), "UI_SET_KEYBIT");

    uinput_setup setup;
    std::memset(&setup, 0, sizeof(setup));
    setup.id.bustype = BUS_VIRTUAL;
    setup.id.vendor = 0x1d6b;  // "Linux Foundation" — arbitrary but recognizable
    setup.id.product = 0x0e57; // "EST"
    setup.id.version = 1;
    std::strncpy(setup.name, kDeviceName, UINPUT_MAX_NAME_SIZE - 1);

    check(ioctl(fd_, UI_DEV_SETUP, &setup), "UI_DEV_SETUP");
    check(ioctl(fd_, UI_DEV_CREATE), "UI_DEV_CREATE");
}

UinputInjector::~UinputInjector() {
    if (fd_ >= 0) {
        ioctl(fd_, UI_DEV_DESTROY);
        ::close(fd_);
    }
}

void UinputInjector::emit(uint16_t type, uint16_t code, int32_t value) {
    input_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    // ev.time is left zero: the kernel timestamps uinput events on receipt.
    if (::write(fd_, &ev, sizeof(ev)) != static_cast<ssize_t>(sizeof(ev)))
        throw std::system_error(errno, std::generic_category(), "uinput write");
}

void UinputInjector::move_relative(double dx, double dy) {
    accum_x_ += dx;
    accum_y_ += dy;
    int ix = static_cast<int>(accum_x_);
    int iy = static_cast<int>(accum_y_);
    accum_x_ -= ix;
    accum_y_ -= iy;
    if (ix)
        emit(EV_REL, REL_X, ix);
    if (iy)
        emit(EV_REL, REL_Y, iy);
    if (ix || iy)
        emit(EV_SYN, SYN_REPORT, 0);
}

void UinputInjector::scroll(int dx, int dy) {
    if (dx)
        emit(EV_REL, REL_HWHEEL, dx);
    if (dy)
        emit(EV_REL, REL_WHEEL, dy);
    if (dx || dy)
        emit(EV_SYN, SYN_REPORT, 0);
}

void UinputInjector::button(Button logical, bool pressed) {
    if (is_wheel_button(logical)) {
        // Wheel "buttons" only act on press; release is a no-op.
        if (!pressed)
            return;
        switch (logical) {
        case 4: scroll(0, 1);  break; // up
        case 5: scroll(0, -1); break; // down
        case 6: scroll(-1, 0); break; // left
        case 7: scroll(1, 0);  break; // right
        }
        return;
    }
    uint16_t code = logical_to_evdev(logical);
    if (!code)
        return; // unknown button
    emit(EV_KEY, code, pressed ? 1 : 0);
    emit(EV_SYN, SYN_REPORT, 0);
}

void UinputInjector::key(uint16_t evdev_keycode, bool pressed) {
    emit(EV_KEY, evdev_keycode, pressed ? 1 : 0);
    emit(EV_SYN, SYN_REPORT, 0);
}

void UinputInjector::flush() {
    emit(EV_SYN, SYN_REPORT, 0);
}

} // namespace es
