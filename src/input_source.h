#pragma once

namespace es {

// Produces normalized input from the platform and delivers it to an InputSink.
//
// Current implementation: LibinputSource (read-only monitoring; works alongside
// the compositor on any seat the user can read).
//
// Planned: EvdevSource — opens selected pointing devices directly with
// EVIOCGRAB so the trigger button can be *suppressed* (and replayed via the
// injector if the motion turns out not to be a gesture). That is the faithful
// Wayland equivalent of X11's passive XIGrabButton, and the reason the input
// engine will eventually run as a privileged daemon.
class InputSource {
public:
    virtual ~InputSource() = default;

    // A pollable file descriptor; becomes readable when events are pending.
    virtual int fd() const = 0;

    // Drain all pending events and deliver them to the sink.
    virtual void dispatch() = 0;
};

} // namespace es
