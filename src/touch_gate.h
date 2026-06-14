#pragma once

namespace es {

// Which screen edge the anchor finger must start from (None = touch disabled).
enum class TouchEdge { None, Left, Right, Top, Bottom };

// Edge-anchored two-finger touch gesture gate. Pure logic, unit-tested.
//
// Interaction: the first finger that lands within `band_px` of the configured
// screen edge becomes the *anchor* (held; it may drag inward, but its own path
// is ignored). The next finger down is the *draw* finger, whose path is the
// stroke. A third finger is ignored. Lifting the draw finger finalizes the
// stroke; lifting the anchor while a stroke is in progress cancels it. The
// anchor stays armed after a finalize, so another stroke can be drawn without
// re-anchoring (lift the anchor to end the session).
//
// The anchor must land *before* the draw finger — a finger that goes down away
// from the edge with no anchor held yet is ignored and never promoted.
class TouchGate {
public:
    enum class Down { Ignore, Anchor, Draw };
    // Finalize: the draw finger lifted (anchor still held, armed for another).
    // Cancel: the anchor lifted mid-stroke. EndSession: the anchor lifted with
    // no stroke in progress. Both anchor lifts end the session (hide any cue).
    enum class Up { Ignore, Cancel, Finalize, EndSession };

    void configure(TouchEdge edge, int screen_w, int screen_h, int band_px) {
        edge_ = edge;
        screen_w_ = screen_w;
        screen_h_ = screen_h;
        band_ = band_px > 0 ? band_px : 1;
        reset();
    }

    bool enabled() const { return edge_ != TouchEdge::None; }

    Down on_down(int slot, double x, double y) {
        if (!enabled())
            return Down::Ignore;
        if (!anchored_) {
            if (!in_band(x, y))
                return Down::Ignore;
            anchored_ = true;
            anchor_slot_ = slot;
            return Down::Anchor;
        }
        if (!drawing_ && slot != anchor_slot_) {
            drawing_ = true;
            draw_slot_ = slot;
            return Down::Draw;
        }
        return Down::Ignore; // third finger, or a duplicate slot
    }

    Up on_up(int slot) {
        if (!enabled())
            return Up::Ignore;
        if (drawing_ && slot == draw_slot_) {
            drawing_ = false;
            draw_slot_ = -1;
            return Up::Finalize; // anchor stays armed for another stroke
        }
        if (anchored_ && slot == anchor_slot_) {
            bool was_drawing = drawing_;
            reset();
            return was_drawing ? Up::Cancel : Up::EndSession;
        }
        return Up::Ignore;
    }

    // True only for the finger whose motion should be recorded as the stroke.
    bool is_draw(int slot) const { return drawing_ && slot == draw_slot_; }
    // True for the held anchor finger (so its motion can move the visual cue).
    bool is_anchor(int slot) const { return anchored_ && slot == anchor_slot_; }

    void reset() {
        anchored_ = false;
        drawing_ = false;
        anchor_slot_ = -1;
        draw_slot_ = -1;
    }

private:
    bool in_band(double x, double y) const {
        switch (edge_) {
        case TouchEdge::Left:
            return x <= band_;
        case TouchEdge::Right:
            return x >= screen_w_ - band_;
        case TouchEdge::Top:
            return y <= band_;
        case TouchEdge::Bottom:
            return y >= screen_h_ - band_;
        case TouchEdge::None:
            return false;
        }
        return false;
    }

    TouchEdge edge_ = TouchEdge::None;
    int screen_w_ = 0;
    int screen_h_ = 0;
    int band_ = 30;
    bool anchored_ = false;
    bool drawing_ = false;
    int anchor_slot_ = -1;
    int draw_slot_ = -1;
};

} // namespace es
