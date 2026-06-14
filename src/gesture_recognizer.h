#pragma once
#include "gesture.h"
#include "input_sink.h"
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
    // Modifiers that must be held when the trigger is pressed (mouse mode).
    void set_required_modifiers(unsigned m) { required_mods_ = m; }
    void set_reporter(std::function<void(const Recognition &)> reporter) {
        reporter_ = std::move(reporter);
    }

    // Optional live trail renderer; nullptr (default) draws nothing.
    void set_overlay(TraceOverlay *overlay) { overlay_ = overlay; }

    void on_button(Button button, bool pressed, Sample at) override;
    void on_motion(Sample at, double dx, double dy) override;
    void on_scroll(double, double, Sample) override {}
    void on_modifiers(unsigned mask) override { cur_mods_ = mask; }

    // Minimum cursor travel (px) before a press+release counts as a gesture
    // rather than a plain click. Matches the X11 default.
    static constexpr double kGestureMinTravel = 16.0;

private:
    Recognition recognize(const Gesture &g) const;

    Button trigger_;
    double threshold_;
    unsigned required_mods_ = 0; // modifiers required at trigger-press (mouse mode)
    unsigned cur_mods_ = 0;      // current modifier state from the source
    bool recording_ = false;
    Point origin_;
    double max_travel_ = 0.0;
    std::vector<Sample> samples_;
    std::vector<GestureBinding> bindings_;
    std::function<void(const Recognition &)> reporter_;
    TraceOverlay *overlay_ = nullptr;
};

} // namespace es
