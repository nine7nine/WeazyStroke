// Verifies the suppress-vs-replay decision logic without touching real devices.

#include "trigger_gate.h"

#include <cassert>
#include <cstdio>

using namespace es;

int main() {
    constexpr double kMin = 16.0;
    using R = TriggerGate::Release;

    // Quick click, no motion -> replay.
    {
        TriggerGate g(kMin);
        g.on_press();
        assert(g.withholding());
        assert(g.on_release() == R::ReplayClick);
        assert(!g.withholding());
    }

    // Jitter below threshold -> still a click.
    {
        TriggerGate g(kMin);
        g.on_press();
        g.on_motion(3);
        g.on_motion(9);
        g.on_motion(5); // max stays 9
        assert(g.max_travel() == 9.0);
        assert(g.on_release() == R::ReplayClick);
    }

    // Clear movement -> gesture, swallow the click.
    {
        TriggerGate g(kMin);
        g.on_press();
        g.on_motion(5);
        g.on_motion(40);
        g.on_motion(20); // max stays 40
        assert(g.max_travel() == 40.0);
        assert(g.on_release() == R::Swallow);
    }

    // Exactly at threshold counts as a gesture.
    {
        TriggerGate g(kMin);
        g.on_press();
        g.on_motion(kMin);
        assert(g.on_release() == R::Swallow);
    }

    // Motion outside an active press is ignored.
    {
        TriggerGate g(kMin);
        g.on_motion(100); // no press in progress
        g.on_press();
        assert(g.on_release() == R::ReplayClick);
    }

    std::printf("trigger_gate_test: PASS\n");
    return 0;
}
