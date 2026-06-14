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
    // Edge-anchored two-finger touch gesture: the screen edge the anchor finger
    // must start from ("none"|"left"|"right"|"top"|"bottom"; "none" disables).
    // A finger held within touch_band px of that edge arms it; a second finger
    // draws the stroke. touch_band is stored under "settings". Defaults to the
    // right edge (left/top/bottom are commonly taken by compositor edge swipes).
    std::string touch_edge = "right";
    int touch_band = 30;     // px from the edge that counts as the anchor zone
    bool touch_cue = true;     // show the edge-hold "armed" ring overlay
    int touch_ring = 90;       // radius (px) of that ring
    int touch_grow_ms = 450;   // ring grow-out duration (ms)
    int touch_out_ms = 220;    // ring shrink+fade-out duration on release (ms)
                               // (touch_band/cue/ring/grow_ms/out_ms live in settings)
    // Tunable settings (easystroke's Preferences). Recognition + feedback knobs
    // the daemon reads; the GUI edits them. Stored under "settings" in the JSON.
    double match_threshold = 0.6; // recognition accuracy floor (0..1)
    int trace_width = 4;          // overlay trail width (px); also the pressure-off width
    bool pressure = true;         // pen pressure varies the trail width
    int pressure_min = 2;         // trail width (px) at lightest pen pressure
    int pressure_max = 14;        // trail width (px) at hardest pen pressure
    // Touch contact-area -> pseudo-pressure (read from the raw touchscreen, since
    // libinput hides it), so two-finger trails also thicken under a fatter touch.
    bool touch_pressure = true;     // use contact size for touch trail width
    // Contact-size band mapped onto the width range: floor -> thinnest, ref ->
    // full width. A narrow real range (touch contact barely varies) is stretched
    // across the width range, so the thin/thick effect is pronounced.
    int touch_pressure_floor = 150; // TOUCH_MAJOR at/below which the trail is thinnest
    int touch_pressure_ref = 500;   // TOUCH_MAJOR mapped to full width (0 = device max)
    double scroll_speed = 1.0;    // multiplier for scroll actions
    bool scroll_invert = false;   // invert scroll-action direction
    bool show_osd = false;          // flash the matched gesture name on screen
    std::string trail_effect = "plain"; // overlay bling: "plain" | "glow" | "sparkle"
    int trail_fade_ms = 380;            // completion fade-out duration (ms; 0 = instant)
    // Trail/ring gradient endpoints (hex). The colour eases start->end along the
    // stroke (defaults blue->green, matching the original easystroke direction).
    std::string trail_color_start = "#0000ff";
    std::string trail_color_end = "#00ff00";
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
