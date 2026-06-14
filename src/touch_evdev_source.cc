#include "touch_evdev_source.h"
#include "input_sink.h"

#include <libevdev/libevdev.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <stdexcept>

namespace es {

namespace {

uint32_t event_time_ms(const input_event &ev) {
    return static_cast<uint32_t>(ev.input_event_sec * 1000 + ev.input_event_usec / 1000);
}

// ESWL_DEBUG_TOUCH=1 enables verbose touch tracing (calibration / debugging).
bool dbg() {
    static const bool on = std::getenv("ESWL_DEBUG_TOUCH") != nullptr;
    return on;
}

// A direct (on-screen) multitouch device with absolute MT position + tracking.
bool is_touchscreen(libevdev *ev) {
    return libevdev_has_property(ev, INPUT_PROP_DIRECT) &&
           libevdev_has_event_code(ev, EV_ABS, ABS_MT_POSITION_X) &&
           libevdev_has_event_code(ev, EV_ABS, ABS_MT_POSITION_Y) &&
           libevdev_has_event_code(ev, EV_ABS, ABS_MT_TRACKING_ID);
}

} // namespace

TouchEvdevSource::TouchEvdevSource(InputSink &sink, int screen_w, int screen_h, double pressure_ref)
    : sink_(sink), screen_w_(screen_w), screen_h_(screen_h), pressure_ref_(pressure_ref) {
    if (!open_touchscreen())
        throw std::runtime_error("no readable touchscreen with MT position found");
}

TouchEvdevSource::~TouchEvdevSource() {
    if (evdev_)
        libevdev_free(evdev_);
    if (fd_ >= 0)
        ::close(fd_);
}

int TouchEvdevSource::fd() const { return fd_; }

bool TouchEvdevSource::open_touchscreen() {
    DIR *dir = ::opendir("/dev/input");
    if (!dir)
        return false;

    // Prefer a touchscreen that reports contact size; fall back to one without.
    std::string best_path;
    bool best_has_major = false;

    while (dirent *e = ::readdir(dir)) {
        if (std::strncmp(e->d_name, "event", 5) != 0)
            continue;
        std::string path = std::string("/dev/input/") + e->d_name;
        int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0)
            continue;
        libevdev *ev = nullptr;
        if (libevdev_new_from_fd(fd, &ev) < 0) {
            ::close(fd);
            continue;
        }
        bool ok = is_touchscreen(ev);
        bool has_major = ok && libevdev_has_event_code(ev, EV_ABS, ABS_MT_TOUCH_MAJOR);
        libevdev_free(ev);
        ::close(fd);
        if (!ok)
            continue;
        if (best_path.empty() || (has_major && !best_has_major)) {
            best_path = path;
            best_has_major = has_major;
            if (has_major)
                break; // good enough
        }
    }
    ::closedir(dir);

    if (best_path.empty())
        return false;

    fd_ = ::open(best_path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd_ < 0)
        return false;
    if (libevdev_new_from_fd(fd_, &evdev_) < 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    const char *nm = libevdev_get_name(evdev_);
    name_ = nm ? nm : best_path;
    xmin_ = libevdev_get_abs_minimum(evdev_, ABS_MT_POSITION_X);
    xmax_ = libevdev_get_abs_maximum(evdev_, ABS_MT_POSITION_X);
    ymin_ = libevdev_get_abs_minimum(evdev_, ABS_MT_POSITION_Y);
    ymax_ = libevdev_get_abs_maximum(evdev_, ABS_MT_POSITION_Y);
    if (libevdev_has_event_code(evdev_, EV_ABS, ABS_MT_TOUCH_MAJOR))
        tmajor_max_ = libevdev_get_abs_maximum(evdev_, ABS_MT_TOUCH_MAJOR);

    // Size the slot table from the device (fall back to a sane default).
    int nslots = libevdev_get_num_slots(evdev_);
    if (nslots < 1)
        nslots = 10;
    slots_.resize(nslots);
    cur_slot_ = libevdev_get_current_slot(evdev_);
    if (cur_slot_ < 0 || cur_slot_ >= nslots)
        cur_slot_ = 0;
    if (dbg())
        std::fprintf(stderr, "[touch] '%s' slots=%d X[%d,%d] Y[%d,%d] major_max=%d screen=%dx%d\n",
                     name_.c_str(), nslots, xmin_, xmax_, ymin_, ymax_, tmajor_max_, screen_w_,
                     screen_h_);
    return true;
}

double TouchEvdevSource::scale_x(int v) const {
    if (xmax_ <= xmin_)
        return v;
    double n = static_cast<double>(v - xmin_) / (xmax_ - xmin_);
    return std::clamp(n, 0.0, 1.0) * screen_w_;
}

double TouchEvdevSource::scale_y(int v) const {
    if (ymax_ <= ymin_)
        return v;
    double n = static_cast<double>(v - ymin_) / (ymax_ - ymin_);
    return std::clamp(n, 0.0, 1.0) * screen_h_;
}

double TouchEvdevSource::scale_pressure(int v) const {
    if (tmajor_max_ <= 0)
        return -1.0; // no contact-size data -> constant width
    // Map the usable contact band [floor, ref] onto 0..1 (fixed, not a running
    // max, so variation within a stroke shows). Stretching the narrow real range
    // across the band makes the thin/thick effect pronounced. Device-specific ->
    // both live in prefs; ref 0 falls back to the device max.
    double hi = pressure_ref_ > 0 ? pressure_ref_ : tmajor_max_;
    double lo = pressure_floor_;
    if (hi <= lo)
        hi = lo + 1.0;
    return std::clamp((static_cast<double>(v) - lo) / (hi - lo), 0.0, 1.0);
}

void TouchEvdevSource::dispatch() { read_events(); }

void TouchEvdevSource::read_events() {
    input_event ev;
    int rc = libevdev_next_event(evdev_, LIBEVDEV_READ_FLAG_NORMAL, &ev);
    while (rc == LIBEVDEV_READ_STATUS_SUCCESS || rc == LIBEVDEV_READ_STATUS_SYNC) {
        if (rc == LIBEVDEV_READ_STATUS_SYNC) {
            while (rc == LIBEVDEV_READ_STATUS_SYNC) // drained on overflow; ignore the resync
                rc = libevdev_next_event(evdev_, LIBEVDEV_READ_FLAG_SYNC, &ev);
            continue;
        }
        if (ev.type == EV_ABS) {
            switch (ev.code) {
            case ABS_MT_SLOT:
                if (ev.value >= 0 && ev.value < static_cast<int>(slots_.size()))
                    cur_slot_ = ev.value;
                break;
            case ABS_MT_TRACKING_ID:
                slots_[cur_slot_].tracking_id = ev.value;
                slots_[cur_slot_].changed = true;
                break;
            case ABS_MT_POSITION_X:
                slots_[cur_slot_].x = scale_x(ev.value);
                slots_[cur_slot_].moved = true;
                break;
            case ABS_MT_POSITION_Y:
                slots_[cur_slot_].y = scale_y(ev.value);
                slots_[cur_slot_].moved = true;
                break;
            case ABS_MT_TOUCH_MAJOR:
                slots_[cur_slot_].pressure = scale_pressure(ev.value);
                if (ev.value > slots_[cur_slot_].peak_major)
                    slots_[cur_slot_].peak_major = ev.value;
                break;
            default:
                break;
            }
        } else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            flush_frame(event_time_ms(ev));
        }
        rc = libevdev_next_event(evdev_, LIBEVDEV_READ_FLAG_NORMAL, &ev);
    }
}

void TouchEvdevSource::flush_frame(uint32_t time_ms) {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
        Slot &s = slots_[i];
        int slot = static_cast<int>(i);
        if (s.changed) {
            s.changed = false;
            if (s.tracking_id >= 0 && !s.down) {
                s.down = true;
                s.peak_major = 0;
                s.motions = 0;
                if (dbg())
                    std::fprintf(stderr, "[touch] down  slot=%d x=%.0f y=%.0f\n", slot, s.x, s.y);
                sink_.on_touch_down(slot, {s.x, s.y, time_ms, s.pressure});
                s.moved = false;
                continue;
            }
            if (s.tracking_id < 0 && s.down) {
                s.down = false;
                if (dbg())
                    std::fprintf(stderr,
                                 "[touch] up    slot=%d x=%.0f y=%.0f motions=%d peak_major=%d -> "
                                 "pressure %.2f\n",
                                 slot, s.x, s.y, s.motions, s.peak_major,
                                 scale_pressure(s.peak_major));
                sink_.on_touch_up(slot, {s.x, s.y, time_ms, s.pressure});
                s.moved = false;
                s.pressure = -1.0;
                continue;
            }
        }
        if (s.down && s.moved) {
            ++s.motions;
            sink_.on_touch_motion(slot, {s.x, s.y, time_ms, s.pressure});
        }
        s.moved = false;
    }
}

} // namespace es
