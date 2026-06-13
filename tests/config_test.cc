// Saves a config to a temp file, reads it back, and checks the round-trip.

#include "gesture_config.h"

#include <cassert>
#include <cstdio>
#include <filesystem>

using namespace es;

int main() {
    namespace fs = std::filesystem;
    fs::path tmp = fs::temp_directory_path() / "eswl_config_test.json";
    fs::remove(tmp);

    // Loading a non-existent file yields defaults.
    GestureConfig def = GestureConfig::load(tmp.string());
    assert(def.gestures.empty());
    assert(def.trigger_button == 3);

    GestureConfig cfg;
    cfg.trigger_button = 8;
    // {name, key, text, command, points}
    cfg.gestures.push_back({"launch-term", "", "", "xterm", {{0, 0}, {1, 2.5}, {3, 4}}});
    cfg.gestures.push_back({"close-tab", "ctrl+w", "", "", {{-1, -1}, {0, 0}, {1, 1}, {2, 2}}});
    cfg.gestures.push_back({"sig", "", "best, me", "", {{0, 0}, {5, 5}, {10, 0}, {15, 5}}});
    cfg.save(tmp.string());

    GestureConfig back = GestureConfig::load(tmp.string());
    assert(back.trigger_button == 8);
    assert(back.gestures.size() == 3);

    assert(back.gestures[0].name == "launch-term");
    assert(back.gestures[0].command == "xterm");
    assert(back.gestures[0].points.size() == 3);
    assert(back.gestures[0].points[1].x == 1.0);
    assert(back.gestures[0].points[1].y == 2.5);

    assert(back.gestures[1].name == "close-tab");
    assert(back.gestures[1].key == "ctrl+w");
    assert(back.gestures[1].command.empty());

    assert(back.gestures[2].text == "best, me");
    assert(back.gestures[2].points.size() == 4);

    fs::remove(tmp);
    std::printf("config_test: PASS\n");
    return 0;
}
