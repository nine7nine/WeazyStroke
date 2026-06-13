#pragma once

namespace es {

// Decides whether a held trigger button is a gesture (suppress it) or a plain
// click (replay it so the app still gets the click). Pure logic, unit-tested.
//
// The grabbed trigger press is withheld from the compositor until release; then
// it is either swallowed (a gesture was drawn) or replayed as a real click.
//
// v0 limitation: there is no press-and-hold timeout, so holding the trigger to
// drag does not pass through until release. That is acceptable for the usual
// right/extra-button triggers and can be added later.
class TriggerGate {
public:
    enum class Release { Swallow, ReplayClick };

    explicit TriggerGate(double min_travel) : min_travel_(min_travel) {}

    void on_press() {
        active_ = true;
        max_travel_ = 0.0;
    }

    // travel_from_origin: cumulative distance of the pointer from the press
    // point, in pixels.
    void on_motion(double travel_from_origin) {
        if (active_ && travel_from_origin > max_travel_)
            max_travel_ = travel_from_origin;
    }

    Release on_release() {
        bool gesture = active_ && max_travel_ >= min_travel_;
        active_ = false;
        return gesture ? Release::Swallow : Release::ReplayClick;
    }

    bool withholding() const { return active_; }
    double max_travel() const { return max_travel_; }

private:
    double min_travel_;
    bool active_ = false;
    double max_travel_ = 0.0;
};

} // namespace es
