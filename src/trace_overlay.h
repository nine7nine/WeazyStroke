#pragma once

namespace es {

// Draws the live stroke trail while a gesture is in progress. Abstract so the
// input engine stays toolkit-free: it talks to an implementation (a separate
// GTK4 layer-shell process; see ProcessOverlay) without linking any GUI library
// itself. Points are delivered in the daemon's screen-pixel space; the renderer
// scales them to the actual output. The default is no overlay at all.
class TraceOverlay {
public:
    virtual ~TraceOverlay() = default;

    virtual void begin() = 0;                 // start a fresh stroke (clear old trail)
    virtual void add(double x, double y) = 0; // append a point (screen-pixel space)
    virtual void end() = 0;                   // gesture finished; clear the trail
};

} // namespace es
