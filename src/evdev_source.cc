#include "evdev_source.h"
#include "button_map.h"
#include "input_injector.h"
#include "input_sink.h"
#include "uinput_injector.h"

#include <libevdev/libevdev.h>

#include <fcntl.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <stdexcept>

namespace es {

namespace {

uint32_t event_time_ms(const input_event &ev) {
    return static_cast<uint32_t>(ev.input_event_sec * 1000 + ev.input_event_usec / 1000);
}

} // namespace

EvdevSource::EvdevSource(InputSink &sink, InputInjector &injector, Button trigger, int screen_w,
                         int screen_h)
    : sink_(sink),
      injector_(injector),
      trigger_(trigger),
      trigger_evdev_(logical_to_evdev(trigger)),
      screen_w_(screen_w),
      screen_h_(screen_h),
      pos_{screen_w / 2.0, screen_h / 2.0},
      gate_(16.0) {
    if (trigger_evdev_ == 0)
        throw std::runtime_error("trigger button is not a grabbable physical button");

    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0)
        throw std::runtime_error("epoll_create1 failed");

    scan_and_grab();

    if (devices_.empty()) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
        throw std::runtime_error("no grabbable mouse devices found");
    }
}

EvdevSource::~EvdevSource() {
    for (auto &d : devices_) {
        if (d->evdev) {
            libevdev_grab(d->evdev, LIBEVDEV_UNGRAB);
            libevdev_free(d->evdev);
        }
        if (d->fd >= 0)
            ::close(d->fd);
    }
    if (epoll_fd_ >= 0)
        ::close(epoll_fd_);
}

int EvdevSource::fd() const { return epoll_fd_; }

bool EvdevSource::looks_like_mouse(libevdev *ev) const {
    // A relative pointer with buttons, carrying the trigger button.
    return libevdev_has_event_type(ev, EV_REL) &&
           libevdev_has_event_code(ev, EV_REL, REL_X) &&
           libevdev_has_event_code(ev, EV_REL, REL_Y) &&
           libevdev_has_event_type(ev, EV_KEY) &&
           libevdev_has_event_code(ev, EV_KEY, trigger_evdev_);
}

void EvdevSource::try_add(const std::string &path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return;

    libevdev *ev = nullptr;
    if (libevdev_new_from_fd(fd, &ev) < 0) {
        ::close(fd);
        return;
    }

    const char *name = libevdev_get_name(ev);
    // Never grab our own injected-output device, and only grab mice.
    if ((name && std::strcmp(name, UinputInjector::kDeviceName) == 0) || !looks_like_mouse(ev)) {
        libevdev_free(ev);
        ::close(fd);
        return;
    }

    if (libevdev_grab(ev, LIBEVDEV_GRAB) < 0) {
        libevdev_free(ev);
        ::close(fd);
        return;
    }

    auto dev = std::make_unique<Device>();
    dev->fd = fd;
    dev->evdev = ev;
    dev->name = name ? name : path;

    epoll_event ee;
    std::memset(&ee, 0, sizeof(ee));
    ee.events = EPOLLIN;
    ee.data.ptr = dev.get();
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ee) < 0) {
        libevdev_grab(ev, LIBEVDEV_UNGRAB);
        libevdev_free(ev);
        ::close(fd);
        return;
    }
    devices_.push_back(std::move(dev));
}

void EvdevSource::scan_and_grab() {
    DIR *dir = ::opendir("/dev/input");
    if (!dir)
        throw std::runtime_error("cannot open /dev/input");
    while (dirent *e = ::readdir(dir)) {
        if (std::strncmp(e->d_name, "event", 5) == 0)
            try_add(std::string("/dev/input/") + e->d_name);
    }
    ::closedir(dir);
}

void EvdevSource::clamp_position() {
    pos_.x = std::clamp(pos_.x, 0.0, static_cast<double>(screen_w_));
    pos_.y = std::clamp(pos_.y, 0.0, static_cast<double>(screen_h_));
}

void EvdevSource::dispatch() {
    epoll_event events[16];
    int n = ::epoll_wait(epoll_fd_, events, 16, 0);
    for (int i = 0; i < n; ++i)
        read_device(*static_cast<Device *>(events[i].data.ptr));
}

void EvdevSource::read_device(Device &d) {
    input_event ev;
    int rc = libevdev_next_event(d.evdev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
    while (rc == LIBEVDEV_READ_STATUS_SUCCESS || rc == LIBEVDEV_READ_STATUS_SYNC) {
        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            // Dropped events: drain the resync stream, ignoring its contents.
            while (rc == LIBEVDEV_READ_STATUS_SYNC)
                rc = libevdev_next_event(d.evdev, LIBEVDEV_READ_FLAG_SYNC, &ev);
            continue;
        }

        switch (ev.type) {
        case EV_REL:
            if (ev.code == REL_X) {
                frame_dx_ += ev.value;
                frame_motion_ = true;
            } else if (ev.code == REL_Y) {
                frame_dy_ += ev.value;
                frame_motion_ = true;
            } else if (ev.code == REL_WHEEL) {
                frame_wheel_ += ev.value;
                frame_wheel_evt_ = true;
            } else if (ev.code == REL_HWHEEL) {
                frame_hwheel_ += ev.value;
                frame_wheel_evt_ = true;
            }
            break;
        case EV_KEY:
            if (ev.value != 2) // ignore autorepeat
                process_button(ev.code, ev.value, event_time_ms(ev));
            break;
        case EV_SYN:
            if (ev.code == SYN_REPORT)
                flush_frame(event_time_ms(ev));
            break;
        default:
            break;
        }

        rc = libevdev_next_event(d.evdev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
    }
}

void EvdevSource::flush_frame(uint32_t time_ms) {
    if (frame_motion_) {
        pos_.x += frame_dx_;
        pos_.y += frame_dy_;
        clamp_position();
        injector_.move_relative(frame_dx_, frame_dy_); // keep the cursor moving
        sink_.on_motion({pos_.x, pos_.y, time_ms}, frame_dx_, frame_dy_);
        double travel = std::hypot(pos_.x - press_origin_.x, pos_.y - press_origin_.y);
        gate_.on_motion(travel);
    }
    if (frame_wheel_evt_) {
        injector_.scroll(frame_hwheel_, frame_wheel_);
        sink_.on_scroll(frame_hwheel_, frame_wheel_, {pos_.x, pos_.y, time_ms});
    }
    frame_dx_ = frame_dy_ = 0.0;
    frame_wheel_ = frame_hwheel_ = 0;
    frame_motion_ = frame_wheel_evt_ = false;
}

void EvdevSource::process_button(uint16_t code, int value, uint32_t time_ms) {
    bool pressed = value == 1;
    Sample at{pos_.x, pos_.y, time_ms};

    if (code == trigger_evdev_) {
        if (pressed) {
            gate_.on_press();
            press_origin_ = pos_;
            sink_.on_button(trigger_, true, at); // withheld from compositor
        } else {
            sink_.on_button(trigger_, false, at); // recognizer may run the action
            if (gate_.on_release() == TriggerGate::Release::ReplayClick) {
                injector_.button(trigger_, true);
                injector_.button(trigger_, false);
            }
            // Swallow: the gesture handled it; the click is suppressed.
        }
        return;
    }

    // Non-trigger button: forward it through and report it.
    Button logical = evdev_to_logical(code);
    if (logical) {
        injector_.button(logical, pressed);
        sink_.on_button(logical, pressed, at);
    } else {
        injector_.key(code, pressed); // unmapped extra button: forward raw
    }
}

} // namespace es
