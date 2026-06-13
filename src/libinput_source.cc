#include "libinput_source.h"
#include "button_map.h"
#include "input_sink.h"

#include <libinput.h>
#include <libudev.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <stdexcept>

namespace es {

namespace {

// libinput opens devices on our behalf through these callbacks. Opening the
// /dev/input/event* nodes directly works as long as the user can read them
// (input group); the privileged daemon will be able to grab them here too.
int open_restricted(const char *path, int flags, void * /*user_data*/) {
    int fd = ::open(path, flags);
    return fd < 0 ? -errno : fd;
}

void close_restricted(int fd, void * /*user_data*/) {
    ::close(fd);
}

const libinput_interface kInterface = {open_restricted, close_restricted};

} // namespace

LibinputSource::LibinputSource(InputSink &sink, int screen_w, int screen_h)
    : sink_(sink),
      screen_w_(screen_w),
      screen_h_(screen_h),
      pos_{screen_w / 2.0, screen_h / 2.0} {
    udev_ = udev_new();
    if (!udev_)
        throw std::runtime_error("udev_new failed");

    li_ = libinput_udev_create_context(&kInterface, nullptr, udev_);
    if (!li_) {
        udev_unref(udev_);
        udev_ = nullptr;
        throw std::runtime_error("libinput_udev_create_context failed");
    }

    if (libinput_udev_assign_seat(li_, "seat0") != 0) {
        libinput_unref(li_);
        li_ = nullptr;
        udev_unref(udev_);
        udev_ = nullptr;
        throw std::runtime_error(
            "libinput_udev_assign_seat failed (need read access to /dev/input/*)");
    }

    // Drain the initial DEVICE_ADDED batch so callers see the device list.
    dispatch();
}

LibinputSource::~LibinputSource() {
    if (li_)
        libinput_unref(li_);
    if (udev_)
        udev_unref(udev_);
}

int LibinputSource::fd() const {
    return libinput_get_fd(li_);
}

void LibinputSource::clamp_position() {
    pos_.x = std::clamp(pos_.x, 0.0, static_cast<double>(screen_w_));
    pos_.y = std::clamp(pos_.y, 0.0, static_cast<double>(screen_h_));
}

void LibinputSource::dispatch() {
    if (libinput_dispatch(li_) != 0)
        return;
    while (libinput_event *ev = libinput_get_event(li_)) {
        handle(ev);
        libinput_event_destroy(ev);
    }
}

Sample LibinputSource::update_tablet_pos(libinput_event_tablet_tool *t) {
    // A tablet tool is an absolute device: each event carries the pen's position
    // over the surface, which libinput maps onto our screen rectangle. We only
    // overwrite an axis that actually changed so non-motion events (button, tip)
    // keep the last known position.
    if (libinput_event_tablet_tool_x_has_changed(t))
        pos_.x = libinput_event_tablet_tool_get_x_transformed(t, screen_w_);
    if (libinput_event_tablet_tool_y_has_changed(t))
        pos_.y = libinput_event_tablet_tool_get_y_transformed(t, screen_h_);
    clamp_position();
    return {pos_.x, pos_.y, libinput_event_tablet_tool_get_time(t)};
}

void LibinputSource::set_pen_held(Button b, bool pressed) {
    auto it = std::find(pen_held_.begin(), pen_held_.end(), b);
    if (pressed) {
        if (it == pen_held_.end())
            pen_held_.push_back(b);
    } else if (it != pen_held_.end()) {
        pen_held_.erase(it);
    }
}

void LibinputSource::handle(libinput_event *ev) {
    switch (libinput_event_get_type(ev)) {
    case LIBINPUT_EVENT_DEVICE_ADDED:
        sink_.on_device_added(libinput_device_get_name(libinput_event_get_device(ev)));
        break;

    case LIBINPUT_EVENT_DEVICE_REMOVED:
        sink_.on_device_removed(libinput_device_get_name(libinput_event_get_device(ev)));
        break;

    case LIBINPUT_EVENT_POINTER_MOTION: {
        libinput_event_pointer *p = libinput_event_get_pointer_event(ev);
        double dx = libinput_event_pointer_get_dx(p);
        double dy = libinput_event_pointer_get_dy(p);
        pos_.x += dx;
        pos_.y += dy;
        clamp_position();
        sink_.on_motion({pos_.x, pos_.y, libinput_event_pointer_get_time(p)}, dx, dy);
        break;
    }

    case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
        libinput_event_pointer *p = libinput_event_get_pointer_event(ev);
        double nx = libinput_event_pointer_get_absolute_x_transformed(p, screen_w_);
        double ny = libinput_event_pointer_get_absolute_y_transformed(p, screen_h_);
        double dx = nx - pos_.x;
        double dy = ny - pos_.y;
        pos_.x = nx;
        pos_.y = ny;
        clamp_position();
        sink_.on_motion({pos_.x, pos_.y, libinput_event_pointer_get_time(p)}, dx, dy);
        break;
    }

    case LIBINPUT_EVENT_POINTER_BUTTON: {
        libinput_event_pointer *p = libinput_event_get_pointer_event(ev);
        uint32_t code = libinput_event_pointer_get_button(p);
        bool pressed =
            libinput_event_pointer_get_button_state(p) == LIBINPUT_BUTTON_STATE_PRESSED;
        Button logical = evdev_to_logical(static_cast<uint16_t>(code));
        if (logical)
            sink_.on_button(logical, pressed,
                            {pos_.x, pos_.y, libinput_event_pointer_get_time(p)});
        break;
    }

    case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL: {
        libinput_event_pointer *p = libinput_event_get_pointer_event(ev);
        double h = 0.0;
        double v = 0.0;
        if (libinput_event_pointer_has_axis(p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL))
            h = libinput_event_pointer_get_scroll_value_v120(
                    p, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL) /
                120.0;
        if (libinput_event_pointer_has_axis(p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL))
            v = libinput_event_pointer_get_scroll_value_v120(
                    p, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL) /
                120.0;
        sink_.on_scroll(h, v, {pos_.x, pos_.y, libinput_event_pointer_get_time(p)});
        break;
    }

    // --- Tablet tool (stylus / pen) -------------------------------------
    // The pen is an absolute device, so libinput reports it as a tablet tool
    // rather than a pointer. We map its motion onto the tracked position and
    // surface its tip/buttons as logical pen buttons (kPenTip / kPenButton),
    // letting the same recognizer drive gestures from a stylus.

    case LIBINPUT_EVENT_TABLET_TOOL_AXIS: {
        libinput_event_tablet_tool *t = libinput_event_get_tablet_tool_event(ev);
        double px = pos_.x;
        double py = pos_.y;
        Sample at = update_tablet_pos(t);
        sink_.on_motion(at, pos_.x - px, pos_.y - py);
        break;
    }

    case LIBINPUT_EVENT_TABLET_TOOL_TIP: {
        libinput_event_tablet_tool *t = libinput_event_get_tablet_tool_event(ev);
        Sample at = update_tablet_pos(t);
        bool down = libinput_event_tablet_tool_get_tip_state(t) == LIBINPUT_TABLET_TOOL_TIP_DOWN;
        set_pen_held(kPenTip, down);
        sink_.on_button(kPenTip, down, at);
        break;
    }

    case LIBINPUT_EVENT_TABLET_TOOL_BUTTON: {
        libinput_event_tablet_tool *t = libinput_event_get_tablet_tool_event(ev);
        Sample at = update_tablet_pos(t);
        uint32_t code = libinput_event_tablet_tool_get_button(t);
        bool pressed =
            libinput_event_tablet_tool_get_button_state(t) == LIBINPUT_BUTTON_STATE_PRESSED;
        Button logical = evdev_to_logical(static_cast<uint16_t>(code));
        if (logical) {
            set_pen_held(logical, pressed);
            sink_.on_button(logical, pressed, at);
        }
        break;
    }

    case LIBINPUT_EVENT_TABLET_TOOL_PROXIMITY: {
        libinput_event_tablet_tool *t = libinput_event_get_tablet_tool_event(ev);
        Sample at = update_tablet_pos(t);
        // On proximity-out the hardware stops reporting buttons, so any tip or
        // button still held would leave a gesture open forever. Synthesize the
        // releases (in press order) so the recognizer finalizes the stroke —
        // this is the common "drew the gesture, then lifted the pen away" case.
        if (libinput_event_tablet_tool_get_proximity_state(t) ==
            LIBINPUT_TABLET_TOOL_PROXIMITY_STATE_OUT) {
            for (Button b : pen_held_)
                sink_.on_button(b, false, at);
            pen_held_.clear();
        }
        // Proximity-in needs nothing more: update_tablet_pos already synced the
        // start position so the first stroke begins where the pen actually is.
        break;
    }

    default:
        break;
    }
}

} // namespace es
