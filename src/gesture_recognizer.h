#pragma once
#include "gesture.h"
#include "input_sink.h"
#include "touch_gate.h"
#include "trace_overlay.h"

#include <functional>
#include <string>
#include <vector>

namespace es {

// A named gesture and the action to run when it matches. A gesture may carry
// several recorded examples ("templates"); the candidate matches if it matches
// any of them (best score wins).
struct GestureBinding {
    std::string name;
    std::vector<Gesture> strokes;
    std::function<void()> action;
};

// Outcome of one stroke attempt — reported for logging / future recording UI.
struct Recognition {
    bool matched = false;
    std::string name;   // best-matching binding (empty if none cleared threshold)
    double score = 0.0; // best score seen, even when below threshold
    int points = 0;     // points in the captured stroke
};

// Captures a stroke while the trigger button is held and matches it on release.
// This is the Wayland-side replacement for the X11 StrokeHandler chain, kept
// deliberately small for bring-up: no timeouts, instant gestures, or click
// replay yet (replay needs the grab/suppress EvdevSource).
class GestureRecognizer final : public InputSink {
public:
    explicit GestureRecognizer(Button trigger, double match_threshold = 0.7);

    void add_binding(GestureBinding binding);
    void clear_bindings() { bindings_.clear(); }
    void set_threshold(double t) { threshold_ = t; }
    // Change the trigger button live (e.g. on config reload).
    void set_trigger(Button b) { trigger_ = b; }
    // Modifiers that must be held when the trigger is pressed (mouse mode).
    void set_required_modifiers(unsigned m) { required_mods_ = m; }
    // A second button that must be held for the trigger to start a gesture, and
    // whose release ends one in progress (0 = none). Used for the pen "tip +
    // side button" chord so the side button alone stays free for other actions.
    void set_gate_button(Button b) { gate_button_ = b; }

    // Enable the edge-anchored two-finger touch gesture: a finger held within
    // `band_px` of `edge` arms it, a second finger draws the stroke. None = off.
    void configure_touch(TouchEdge edge, int screen_w, int screen_h, int band_px) {
        touch_.configure(edge, screen_w, screen_h, band_px);
    }

    // Debounce the trigger release by `ms` (0 = off). For the pen tip, which
    // chatters under light pressure: a release that is followed by a press
    // within the window is treated as one continuous stroke, not a new gesture.
    void set_debounce(unsigned ms) { debounce_ms_ = ms; }

    // Must be called periodically (from the daemon's poll loop) so a debounced
    // release finalizes once the window has elapsed with no re-press.
    void tick(uint32_t now_ms);
    void set_reporter(std::function<void(const Recognition &)> reporter) {
        reporter_ = std::move(reporter);
    }

    // Optional live trail renderer; nullptr (default) draws nothing.
    void set_overlay(TraceOverlay *overlay) { overlay_ = overlay; }

    void on_button(Button button, bool pressed, Sample at) override;
    void on_motion(Sample at, double dx, double dy) override;
    void on_scroll(double, double, Sample) override {}
    void on_modifiers(unsigned mask) override { cur_mods_ = mask; }
    void on_touch_down(int slot, Sample at) override;
    void on_touch_motion(int slot, Sample at) override;
    void on_touch_up(int slot, Sample at) override;

    // Minimum cursor travel (px) before a press+release counts as a gesture
    // rather than a plain click. Matches the X11 default.
    static constexpr double kGestureMinTravel = 16.0;

private:
    Recognition recognize(const Gesture &g) const;
    void finalize(); // end the current stroke and run recognition
    // Score a captured stroke and run the matching binding (shared by the button
    // and touch paths). Strokes below the travel/point minimum report no match.
    void run_stroke(const std::vector<Sample> &samples, double max_travel);

    Button trigger_;
    double threshold_;
    unsigned required_mods_ = 0; // modifiers required at trigger-press (mouse mode)
    unsigned cur_mods_ = 0;      // current modifier state from the source
    Button gate_button_ = 0;     // 2nd button that must be held (0 = none)
    bool gate_held_ = false;     // current gate-button state
    unsigned debounce_ms_ = 0;   // trigger-release debounce window (0 = off)
    bool pending_end_ = false;   // a debounced release is waiting to finalize
    uint32_t end_deadline_ = 0;  // when the pending release finalizes (ms)
    bool recording_ = false;
    Point origin_;
    double max_travel_ = 0.0;
    std::vector<Sample> samples_;
    // Edge-anchored two-finger touch path (independent of the button path above).
    TouchGate touch_;
    std::vector<Sample> touch_samples_;
    Point touch_origin_;
    double touch_travel_ = 0.0;
    std::vector<GestureBinding> bindings_;
    std::function<void(const Recognition &)> reporter_;
    TraceOverlay *overlay_ = nullptr;
};

} // namespace es
