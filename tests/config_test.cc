// Saves a config to a temp file, reads it back, and checks the round-trip,
// including multi-example gestures and legacy single-"points" configs.

#include "gesture_config.h"
#include "json.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>

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

    GestureEntry term;
    term.name = "launch-term";
    term.command = "xterm";
    term.strokes = {{{0, 0}, {1, 2.5}, {3, 4}}};
    cfg.gestures.push_back(term);

    GestureEntry tab;
    tab.name = "close-tab";
    tab.key = "ctrl+w";
    tab.strokes = {{{-1, -1}, {0, 0}, {1, 1}, {2, 2}}};
    cfg.gestures.push_back(tab);

    // A gesture with two recorded examples.
    GestureEntry multi;
    multi.name = "swipe";
    multi.text = "best, me";
    multi.strokes = {{{0, 0}, {5, 0}, {10, 0}}, {{0, 0}, {0, 5}, {0, 10}, {0, 15}}};
    cfg.gestures.push_back(multi);

    cfg.save(tmp.string());

    GestureConfig back = GestureConfig::load(tmp.string());
    assert(back.trigger_button == 8);
    assert(back.gestures.size() == 3);

    assert(back.gestures[0].name == "launch-term");
    assert(back.gestures[0].command == "xterm");
    assert(back.gestures[0].strokes.size() == 1);
    assert(back.gestures[0].strokes[0].size() == 3);
    assert(back.gestures[0].strokes[0][1].x == 1.0);
    assert(back.gestures[0].strokes[0][1].y == 2.5);

    assert(back.gestures[1].name == "close-tab");
    assert(back.gestures[1].key == "ctrl+w");
    assert(back.gestures[1].command.empty());

    assert(back.gestures[2].text == "best, me");
    assert(back.gestures[2].strokes.size() == 2);
    assert(back.gestures[2].strokes[0].size() == 3);
    assert(back.gestures[2].strokes[1].size() == 4);

    // Legacy compatibility: a config written with a single "points" array (the
    // pre-multi-example format) still loads as one stroke.
    {
        json::Array pts;
        for (int i = 0; i < 3; ++i) {
            json::Array xy;
            xy.push_back(json::Value(static_cast<double>(i)));
            xy.push_back(json::Value(static_cast<double>(i)));
            pts.push_back(json::Value(std::move(xy)));
        }
        json::Object g;
        g["name"] = json::Value("legacy");
        g["command"] = json::Value("true");
        g["points"] = json::Value(std::move(pts));
        json::Array gs;
        gs.push_back(json::Value(std::move(g)));
        json::Object root;
        root["gestures"] = json::Value(std::move(gs));
        std::ofstream(tmp.string(), std::ios::trunc) << json::Value(std::move(root)).dump();
    }
    GestureConfig legacy = GestureConfig::load(tmp.string());
    assert(legacy.gestures.size() == 1);
    assert(legacy.gestures[0].name == "legacy");
    assert(legacy.gestures[0].strokes.size() == 1);
    assert(legacy.gestures[0].strokes[0].size() == 3);

    fs::remove(tmp);
    std::printf("config_test: PASS\n");
    return 0;
}
