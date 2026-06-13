// Verifies that a gesture with several recorded examples matches a candidate
// close to ANY of them — and that the same candidate is rejected when the
// gesture knows only the other example.

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

// Drives a stroke through the recognizer as if drawn while the trigger is held,
// returning the reported recognition.
Recognition feed(GestureRecognizer &r, Button trigger, const std::vector<Point> &pts) {
    Recognition got;
    r.set_reporter([&](const Recognition &rec) { got = rec; });
    std::uint32_t t = 0;
    r.on_button(trigger, true, {pts.front().x, pts.front().y, t++});
    for (const Point &p : pts)
        r.on_motion({p.x, p.y, t++}, 0, 0);
    r.on_button(trigger, false, {pts.back().x, pts.back().y, t++});
    return got;
}

} // namespace

int main() {
    const Button kTrigger = 11;
    std::vector<Point> hline = line(0, 0, 200, 0, 30);
    std::vector<Point> vline = line(0, 0, 0, 200, 30);

    // Single example (horizontal): matches a horizontal draw, rejects vertical.
    {
        GestureRecognizer r(kTrigger, 0.6);
        r.add_binding({"swipe", {Gesture::from_points(hline)}, [] {}});
        assert(feed(r, kTrigger, hline).matched);
        assert(!feed(r, kTrigger, vline).matched);
    }

    // Two examples (horizontal + vertical): the same gesture now matches both.
    {
        GestureRecognizer r(kTrigger, 0.6);
        r.add_binding(
            {"swipe", {Gesture::from_points(hline), Gesture::from_points(vline)}, [] {}});
        Recognition h = feed(r, kTrigger, hline);
        Recognition v = feed(r, kTrigger, vline);
        assert(h.matched && h.name == "swipe");
        assert(v.matched && v.name == "swipe");
    }

    std::printf("multitemplate_test: PASS\n");
    return 0;
}
