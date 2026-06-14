// Verifies the edge-anchor / draw-finger state machine without real hardware.

#include "touch_gate.h"

#include <cassert>
#include <cstdio>

using namespace es;

int main() {
    using D = TouchGate::Down;
    using U = TouchGate::Up;
    constexpr int W = 1000, H = 800, BAND = 30;

    // Disabled (no edge): every touch is ignored.
    {
        TouchGate g;
        g.configure(TouchEdge::None, W, H, BAND);
        assert(!g.enabled());
        assert(g.on_down(0, 990, 400) == D::Ignore);
        assert(g.on_up(0) == U::Ignore);
    }

    // Right edge: anchor must land in the band; a finger away from it is ignored.
    {
        TouchGate g;
        g.configure(TouchEdge::Right, W, H, BAND);
        assert(g.enabled());
        assert(g.on_down(0, 500, 400) == D::Ignore); // middle of screen, no anchor
        assert(g.on_down(1, 985, 400) == D::Anchor);  // within 30px of right edge
    }

    // Full happy path: anchor, draw, draw finalizes; anchor stays armed.
    {
        TouchGate g;
        g.configure(TouchEdge::Right, W, H, BAND);
        assert(g.on_down(0, 995, 400) == D::Anchor);
        assert(g.is_anchor(0));
        assert(g.on_down(1, 500, 400) == D::Draw);
        assert(g.is_draw(1));
        assert(!g.is_draw(0)); // the anchor never draws
        assert(!g.is_anchor(1)); // ...and the draw finger is not the anchor
        assert(g.on_up(1) == U::Finalize);
        assert(!g.is_draw(1));
        // Anchor still held -> a second stroke can be drawn without re-anchoring.
        assert(g.on_down(2, 480, 420) == D::Draw);
        assert(g.is_draw(2));
        assert(g.on_up(2) == U::Finalize);
    }

    // A third finger during a draw is ignored (single-stroke MVP).
    {
        TouchGate g;
        g.configure(TouchEdge::Right, W, H, BAND);
        assert(g.on_down(0, 995, 400) == D::Anchor);
        assert(g.on_down(1, 500, 400) == D::Draw);
        assert(g.on_down(2, 300, 300) == D::Ignore);
        assert(!g.is_draw(2));
        assert(g.on_up(2) == U::Ignore);
    }

    // Anchor lifts mid-stroke -> cancel (no finalize).
    {
        TouchGate g;
        g.configure(TouchEdge::Right, W, H, BAND);
        assert(g.on_down(0, 995, 400) == D::Anchor);
        assert(g.on_down(1, 500, 400) == D::Draw);
        assert(g.on_up(0) == U::Cancel);
        assert(!g.is_draw(1)); // session reset; the stale draw slot no longer draws
    }

    // Anchor lifts with no draw in progress -> ends the session (no cancel).
    {
        TouchGate g;
        g.configure(TouchEdge::Right, W, H, BAND);
        assert(g.on_down(0, 995, 400) == D::Anchor);
        assert(g.on_up(0) == U::EndSession);
        assert(!g.is_anchor(0)); // session reset
    }

    // The draw finger must come down *after* the anchor: a finger placed before
    // any anchor is ignored and never retroactively promoted.
    {
        TouchGate g;
        g.configure(TouchEdge::Right, W, H, BAND);
        assert(g.on_down(0, 500, 400) == D::Ignore); // draw-ish finger, but no anchor yet
        assert(g.on_down(1, 995, 400) == D::Anchor);
        assert(!g.is_draw(0)); // finger 0 was dropped, not the draw finger
        assert(g.on_down(2, 480, 400) == D::Draw); // a fresh finger becomes the stroke
    }

    // Edge geometry: each edge arms only within its own band.
    {
        TouchGate g;
        g.configure(TouchEdge::Left, W, H, BAND);
        assert(g.on_down(0, 10, 400) == D::Anchor);
    }
    {
        TouchGate g;
        g.configure(TouchEdge::Top, W, H, BAND);
        assert(g.on_down(0, 400, 5) == D::Anchor);
    }
    {
        TouchGate g;
        g.configure(TouchEdge::Bottom, W, H, BAND);
        assert(g.on_down(0, 400, 790) == D::Anchor);
        assert(g.on_down(1, 400, 5) == D::Draw); // anywhere-but-anchor draws
    }

    std::printf("touch_gate_test: PASS\n");
    return 0;
}
