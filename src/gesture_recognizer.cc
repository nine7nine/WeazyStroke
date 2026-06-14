#include "gesture_recognizer.h"

#include <cmath>

namespace es {

GestureRecognizer::GestureRecognizer(Button trigger, double match_threshold)
    : trigger_(trigger), threshold_(match_threshold) {}

void GestureRecognizer::add_binding(GestureBinding binding) {
    bindings_.push_back(std::move(binding));
}

void GestureRecognizer::on_button(Button button, bool pressed, Sample at) {
    // Gate button (e.g. the pen side button in "tip + side button" mode): track
    // its state. Releasing it ends an in-progress chord gesture immediately.
    if (gate_button_ != 0 && button == gate_button_) {
        gate_held_ = pressed;
        if (!pressed && recording_)
            finalize();
        return;
    }

    if (button != trigger_)
        return;

    if (pressed) {
        // A re-press within the debounce window resumes the same stroke (the
        // pen tip "chattered"), rather than starting a new gesture.
        if (recording_ && pending_end_) {
            pending_end_ = false;
            return;
        }
        // Mouse mode: only start if the required modifiers are held.
        if ((cur_mods_ & required_mods_) != required_mods_)
            return;
        // Chord mode: the gate button (e.g. pen side button) must already be held.
        if (gate_button_ != 0 && !gate_held_)
            return;
        recording_ = true;
        pending_end_ = false;
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
    if (debounce_ms_ > 0) {
        // Defer finalizing in case the tip presses again (chatter).
        pending_end_ = true;
        end_deadline_ = at.time_ms + debounce_ms_;
        return;
    }
    finalize();
}

void GestureRecognizer::tick(uint32_t now_ms) {
    if (pending_end_ && static_cast<int32_t>(now_ms - end_deadline_) >= 0)
        finalize();
}

void GestureRecognizer::finalize() {
    recording_ = false;
    pending_end_ = false;
    if (overlay_)
        overlay_->end();
    run_stroke(samples_, max_travel_);
}

void GestureRecognizer::run_stroke(const std::vector<Sample> &samples, double max_travel) {
    // Too little travel => this was a click/tap, not a gesture.
    if (max_travel < kGestureMinTravel || samples.size() <= 2) {
        if (reporter_)
            reporter_(Recognition{false, {}, 0.0, static_cast<int>(samples.size())});
        return;
    }

    Gesture g = Gesture::from_samples(samples);
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

// --- Edge-anchored two-finger touch -------------------------------------
// The TouchGate owns the anchor/draw slot policy; here we only accumulate the
// draw finger's path and reuse the same scoring/dispatch as the button path.

void GestureRecognizer::on_touch_down(int slot, Sample at) {
    switch (touch_.on_down(slot, at.x, at.y)) {
    case TouchGate::Down::Anchor:
        if (overlay_ && touch_cue_)
            overlay_->anchor_show(at.x, at.y); // "armed" ring at the held point
        break;
    case TouchGate::Down::Draw:
        touch_samples_.clear();
        touch_samples_.push_back(at);
        touch_origin_ = {at.x, at.y};
        touch_travel_ = 0.0;
        if (overlay_) {
            overlay_->begin();
            overlay_->add(at.x, at.y);
        }
        break;
    case TouchGate::Down::Ignore:
        break;
    }
}

void GestureRecognizer::on_touch_motion(int slot, Sample at) {
    if (touch_.is_anchor(slot)) {
        if (overlay_ && touch_cue_)
            overlay_->anchor_show(at.x, at.y); // ring follows the held finger
        return;
    }
    if (!touch_.is_draw(slot))
        return;
    touch_samples_.push_back(at);
    if (overlay_)
        overlay_->add(at.x, at.y);
    double travel = std::hypot(at.x - touch_origin_.x, at.y - touch_origin_.y);
    if (travel > touch_travel_)
        touch_travel_ = travel;
}

void GestureRecognizer::on_touch_up(int slot, Sample) {
    switch (touch_.on_up(slot)) {
    case TouchGate::Up::Finalize: // draw finger up; anchor still held -> keep cue
        if (overlay_)
            overlay_->end();
        run_stroke(touch_samples_, touch_travel_);
        touch_samples_.clear();
        break;
    case TouchGate::Up::Cancel: // anchor lifted mid-stroke -> drop trail + cue
        if (overlay_) {
            overlay_->end();
            overlay_->anchor_hide();
        }
        touch_samples_.clear();
        break;
    case TouchGate::Up::EndSession: // anchor lifted, no stroke -> just drop cue
        if (overlay_)
            overlay_->anchor_hide();
        break;
    case TouchGate::Up::Ignore:
        break;
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
        for (const Gesture &tmpl : b.strokes) {
            double score = Gesture::compare(g, tmpl);
            if (score > best.score) {
                best.score = score;
                best.name = b.name;
            }
        }
    }
    best.matched = best.score >= threshold_;
    if (!best.matched)
        best.name.clear();
    return best;
}

} // namespace es
