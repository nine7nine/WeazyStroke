#pragma once
#include "input_event.h"
#include "input_source.h"

#include <vector>

// Opaque libinput / udev types — full definitions live only in the .cc.
struct libinput;
struct libinput_event;
struct libinput_event_tablet_tool;
struct udev;

namespace es {

class InputSink;

// Reads pointer input through libinput's udev backend and forwards normalized
// events to an InputSink. This is a *monitoring* source: it does not grab
// devices, so normal input still reaches the compositor. It therefore cannot
// suppress the trigger button — that is the job of the future EvdevSource.
class LibinputSource final : public InputSource {
public:
    // screen_w/screen_h bound the tracked pointer position and scale absolute
    // devices (tablets/touch). For now they are supplied by the caller; later
    // they will come from the compositor's output geometry.
    LibinputSource(InputSink &sink, int screen_w, int screen_h);
    ~LibinputSource() override;

    LibinputSource(const LibinputSource &) = delete;
    LibinputSource &operator=(const LibinputSource &) = delete;

    int fd() const override;
    void dispatch() override;

    // The pointer position integrated from relative motion (Wayland never tells
    // a client where the cursor is, so we track it ourselves).
    Point position() const { return pos_; }

private:
    void handle(libinput_event *ev);
    void clamp_position();

    // Updates the tracked position from a tablet-tool event's absolute axes
    // (scaled to screen) and returns the timestamped sample.
    Sample update_tablet_pos(libinput_event_tablet_tool *t);

    // Records a pen button/tip as pressed or released so we can synthesize the
    // matching release if the pen leaves proximity (the hardware stops sending
    // button events out of range, which would otherwise hang a gesture open).
    void set_pen_held(Button b, bool pressed);

    InputSink &sink_;
    libinput *li_ = nullptr;
    udev *udev_ = nullptr;
    int screen_w_;
    int screen_h_;
    Point pos_;
    std::vector<Button> pen_held_; // pen buttons/tip currently down (in proximity)
};

} // namespace es
