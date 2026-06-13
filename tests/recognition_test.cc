// Verifies the recycled recognition core works in the new Boost-free project.
// Feeds synthetic strokes (no live input session needed) and checks scoring.

#include "gesture.h"

#include <cassert>
#include <cmath>
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

} // namespace

int main() {
    Gesture right = Gesture::from_points(line(0, 0, 100, 0, 20));
    Gesture down = Gesture::from_points(line(0, 0, 0, 100, 20));
    assert(right.valid() && down.valid());

    // A noisy near-horizontal stroke must match 'right' strongly, 'down' weakly.
    std::vector<Point> q;
    for (int i = 0; i <= 20; ++i) {
        double t = i / 20.0;
        q.push_back({t * 100.0, std::sin(t * M_PI) * 3.0});
    }
    Gesture query = Gesture::from_points(q);

    double s_right = Gesture::compare(query, right);
    double s_down = Gesture::compare(query, down);
    std::printf("noisy-horizontal vs right = %.3f, vs down = %.3f\n", s_right, s_down);
    assert(s_right > 0.7);
    assert(s_right > s_down);

    // Self-similarity is near-perfect.
    double s_self = Gesture::compare(right, right);
    std::printf("self score = %.3f\n", s_self);
    assert(s_self > 0.95);

    // Fewer than three points => invalid, never matches.
    Gesture tiny = Gesture::from_points({{0, 0}, {1, 1}});
    assert(!tiny.valid());
    assert(Gesture::compare(tiny, right) == 0.0);

    std::printf("recognition_test: PASS\n");
    return 0;
}
