#include "gesture_recognizer.h"

#include <cmath>

namespace es {

GestureRecognizer::GestureRecognizer(Button trigger, double match_threshold)
    : trigger_(trigger), threshold_(match_threshold) {}

void GestureRecognizer::add_binding(GestureBinding binding) {
    bindings_.push_back(std::move(binding));
}

void GestureRecognizer::on_button(Button button, bool pressed, Sample at) {
    if (button != trigger_)
        return;

    if (pressed) {
        recording_ = true;
        samples_.clear();
        samples_.push_back(at);
        origin_ = {at.x, at.y};
        max_travel_ = 0.0;
        if (overlay_) {
            overlay_->begin();
            overlay_->add(at.x, at.y);
        }
        return;
    }

    if (!recording_)
        return;
    recording_ = false;
    if (overlay_)
        overlay_->end();

    // Too little travel => this was a click, not a gesture.
    if (max_travel_ < kGestureMinTravel || samples_.size() <= 2) {
        if (reporter_)
            reporter_(Recognition{false, {}, 0.0, static_cast<int>(samples_.size())});
        return;
    }

    Gesture g = Gesture::from_samples(samples_);
    Recognition result = recognize(g);
    if (reporter_)
        reporter_(result);

    if (result.matched) {
        for (const GestureBinding &b : bindings_)
            if (b.name == result.name) {
                if (b.action)
                    b.action();
                break;
            }
    }
}

void GestureRecognizer::on_motion(Sample at, double, double) {
    if (!recording_)
        return;
    samples_.push_back(at);
    if (overlay_)
        overlay_->add(at.x, at.y);
    double travel = std::hypot(at.x - origin_.x, at.y - origin_.y);
    if (travel > max_travel_)
        max_travel_ = travel;
}

Recognition GestureRecognizer::recognize(const Gesture &g) const {
    Recognition best;
    best.points = g.size();
    if (!g.valid())
        return best;

    for (const GestureBinding &b : bindings_) {
        double score = Gesture::compare(g, b.stroke);
        if (score > best.score) {
            best.score = score;
            best.name = b.name;
        }
    }
    best.matched = best.score >= threshold_;
    if (!best.matched)
        best.name.clear();
    return best;
}

} // namespace es
