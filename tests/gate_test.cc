// Verifies the "pen tip + side button" chord gate: a gesture only records while
// the gate button is held, and releasing the gate ends an in-progress stroke.

#include "gesture.h"
#include "gesture_recognizer.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace es;

namespace {

std::vector<Point> line(double x0, double y0, double x1, double y1, int n) {
    std::vector<Point> v;
    for (int i = 0; i <= n; ++i) {
        double t = static_cast<double>(i) / n;
        v.push_back({x0 + (x1 - x0) * t, y0 + (y1 - y0) * t});
    }
    return v;
}

GestureRecognizer make(const std::vector<Point> &tmpl, Button tip, Button gate) {
    GestureRecognizer r(tip, 0.6);
    r.add_binding({"swipe", {Gesture::from_points(tmpl)}, [] {}});
    r.set_gate_button(gate);
    return r;
}

} // namespace

int main() {
    const Button kTip = 10, kSide = 11;
    std::vector<Point> hline = line(0, 0, 200, 0, 30);

    // Tip alone (gate not held) -> never records; the reporter never fires.
    {
        GestureRecognizer r = make(hline, kTip, kSide);
        Recognition got{true, "sentinel", 1.0, 99};
        r.set_reporter([&](const Recognition &rec) { got = rec; });
        std::uint32_t t = 0;
        r.on_button(kTip, true, {hline.front().x, hline.front().y, t++});
        for (const Point &p : hline)
            r.on_motion({p.x, p.y, t++}, 0, 0);
        r.on_button(kTip, false, {hline.back().x, hline.back().y, t++});
        assert(got.points == 99 && got.name == "sentinel"); // untouched -> no report
    }

    // Side held first, then tip drawn -> the chord records and matches.
    {
        GestureRecognizer r = make(hline, kTip, kSide);
        Recognition got;
        r.set_reporter([&](const Recognition &rec) { got = rec; });
        std::uint32_t t = 0;
        r.on_button(kSide, true, {hline.front().x, hline.front().y, t++});
        r.on_button(kTip, true, {hline.front().x, hline.front().y, t++});
        for (const Point &p : hline)
            r.on_motion({p.x, p.y, t++}, 0, 0);
        r.on_button(kTip, false, {hline.back().x, hline.back().y, t++});
        assert(got.matched && got.name == "swipe");
    }

    // Releasing the gate mid-stroke finalizes the gesture (no tip release needed).
    {
        GestureRecognizer r = make(hline, kTip, kSide);
        Recognition got;
        r.set_reporter([&](const Recognition &rec) { got = rec; });
        std::uint32_t t = 0;
        r.on_button(kSide, true, {hline.front().x, hline.front().y, t++});
        r.on_button(kTip, true, {hline.front().x, hline.front().y, t++});
        for (const Point &p : hline)
            r.on_motion({p.x, p.y, t++}, 0, 0);
        r.on_button(kSide, false, {hline.back().x, hline.back().y, t++});
        assert(got.matched && got.name == "swipe");
    }

    std::printf("gate_test: PASS\n");
    return 0;
}
