#pragma once
#include "input_event.h"
#include "stroke.h"

#include <memory>
#include <vector>

namespace es {

// A normalized gesture stroke: a thin, dependency-free C++ wrapper over the C
// recognition core (stroke.c). Deliberately uses only the standard library --
// no Boost, no gdkmm, no X11 -- so the engine stays self-contained.
//
// (The X11 version's Stroke also carried gdkmm thumbnail drawing and Boost
// serialization; both are GUI/persistence concerns and live elsewhere.)
class Gesture {
public:
    Gesture() = default;

    // Build a normalized stroke from raw points. Returns an invalid Gesture if
    // there are too few points (matches the X11 "valid" rule: > 2 points).
    static Gesture from_points(const std::vector<Point> &points);
    static Gesture from_samples(const std::vector<Sample> &samples);

    bool valid() const { return static_cast<bool>(stroke_); }
    int size() const { return stroke_ ? stroke_get_size(stroke_.get()) : 0; }

    // Similarity in [0, 1]; 0 means "no match". Mirrors the X11 Stroke::compare
    // scoring (score = max(1 - 2.5*cost, 0)); the match threshold is applied by
    // the caller.
    static double compare(const Gesture &a, const Gesture &b);

    const stroke_t *raw() const { return stroke_.get(); }

private:
    std::shared_ptr<stroke_t> stroke_;
};

} // namespace es
