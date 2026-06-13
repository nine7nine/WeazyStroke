#include "gesture.h"

#include <algorithm>

namespace es {

namespace {

std::shared_ptr<stroke_t> build(const std::vector<Point> &points) {
    // The X11 PreStroke::valid() rule: a stroke needs more than two points.
    if (points.size() <= 2)
        return nullptr;
    stroke_t *s = stroke_alloc(static_cast<int>(points.size()));
    for (const Point &p : points)
        stroke_add_point(s, p.x, p.y);
    stroke_finish(s);
    return std::shared_ptr<stroke_t>(s, &stroke_free);
}

} // namespace

Gesture Gesture::from_points(const std::vector<Point> &points) {
    Gesture g;
    g.stroke_ = build(points);
    return g;
}

Gesture Gesture::from_samples(const std::vector<Sample> &samples) {
    std::vector<Point> points;
    points.reserve(samples.size());
    for (const Sample &s : samples)
        points.push_back({s.x, s.y});
    return from_points(points);
}

double Gesture::compare(const Gesture &a, const Gesture &b) {
    if (!a.stroke_ || !b.stroke_)
        return 0.0;
    double cost = stroke_compare(a.stroke_.get(), b.stroke_.get(), nullptr, nullptr);
    if (cost >= stroke_infinity)
        return 0.0;
    return std::max(1.0 - 2.5 * cost, 0.0);
}

} // namespace es
