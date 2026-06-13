#pragma once
#include "input_event.h"

#include <string>
#include <vector>

namespace es {

// One persisted gesture: its recorded stroke and the action to run on match.
// Exactly one action field is used, checked in priority order: key, then text,
// then command.
struct GestureEntry {
    std::string name;
    std::string key;     // key combo, e.g. "ctrl+shift+t"
    std::string text;    // literal text to type
    std::string command; // shell command to run
    // One or more recorded examples of the gesture. A candidate matches if it
    // matches ANY example (best score wins), so recording the same gesture a
    // few times makes recognition sturdier.
    std::vector<std::vector<Point>> strokes;
};

// The on-disk configuration. Self-contained JSON; no Boost.
struct GestureConfig {
    Button trigger_button = 3; // X11 easystroke default (right button)
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
