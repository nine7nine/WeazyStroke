#pragma once
#include "input_event.h"

#include <string>
#include <vector>

namespace es {

// One persisted gesture: its recorded stroke and the action to run on match.
// Exactly one action field is used, checked in priority order: key, then text,
// then command.
// One bound gesture, modelled on easystroke's typed actions: a single `type`
// plus an `argument` whose meaning depends on it:
//   "command" -> shell command line (launch apps, scripts)
//   "key"     -> key combo, e.g. "ctrl+shift+t"
//   "text"    -> literal text to type
//   "button"  -> mouse button number to click (1 left, 2 mid, 3 right, ...)
//   "scroll"  -> direction + optional count, e.g. "down" or "down 3"
//   "ignore"  -> explicit no-op (recognized, but does nothing)
//   ""        -> no action bound yet
struct GestureEntry {
    std::string name;
    std::string type;
    std::string argument;
    // One or more recorded examples of the gesture. A candidate matches if it
    // matches ANY example (best score wins), so recording the same gesture a
    // few times makes recognition sturdier.
    std::vector<std::vector<Point>> strokes;
};

// The on-disk configuration. Self-contained JSON; no Boost.
struct GestureConfig {
    Button trigger_button = 3; // X11 easystroke default (right button)
    // Gesture activation mode: "stylus" (pen tip/side button), "mouse" (a mouse
    // button, optionally with modifiers), or "multitouch" (future).
    std::string mode = "stylus";
    unsigned trigger_modifiers = 0; // required modifiers for "mouse" mode (Mod bits)
    // Optional second button that must be held for the trigger to start a
    // gesture (0 = none). Used for the pen "tip + side button" chord, which
    // frees the side button alone for other actions (e.g. a right-click).
    unsigned gate_button = 0;
    // Tunable settings (easystroke's Preferences). Recognition + feedback knobs
    // the daemon reads; the GUI edits them. Stored under "settings" in the JSON.
    double match_threshold = 0.6; // recognition accuracy floor (0..1)
    int trace_width = 4;          // overlay trail width (px)
    double scroll_speed = 1.0;    // multiplier for scroll actions
    bool scroll_invert = false;   // invert scroll-action direction
    bool show_osd = false;          // flash the matched gesture name on screen
    std::string trail_effect = "plain"; // overlay bling: "plain" | "glow" | "sparkle"
    int trail_fade_ms = 380;            // completion fade-out duration (ms; 0 = instant)
    std::vector<GestureEntry> gestures;

    // Loads from `path`. Returns a default config if the file does not exist;
    // throws std::runtime_error / json::ParseError on a malformed file.
    static GestureConfig load(const std::string &path);

    // Writes to `path`, creating parent directories as needed.
    void save(const std::string &path) const;

    // ~/.config/easystroke-wayland/gestures.json (honoring XDG_CONFIG_HOME).
    static std::string default_path();
};

} // namespace es
