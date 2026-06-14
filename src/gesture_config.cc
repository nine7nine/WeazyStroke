#include "gesture_config.h"
#include "json.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace es {

namespace fs = std::filesystem;

std::string GestureConfig::default_path() {
    fs::path base;
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        base = xdg;
    else if (const char *home = std::getenv("HOME"); home && *home)
        base = fs::path(home) / ".config";
    else
        base = ".";
    return (base / "easystroke-wayland" / "gestures.json").string();
}

GestureConfig GestureConfig::load(const std::string &path) {
    GestureConfig cfg;
    if (!fs::exists(path))
        return cfg;

    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("cannot open config: " + path);
    std::stringstream ss;
    ss << in.rdbuf();

    json::Value root = json::Value::parse(ss.str());

    if (root["trigger_button"].is_number())
        cfg.trigger_button = static_cast<Button>(root["trigger_button"].as_number());
    if (root["mode"].is_string())
        cfg.mode = root["mode"].as_string();
    if (root["trigger_modifiers"].is_number())
        cfg.trigger_modifiers = static_cast<unsigned>(root["trigger_modifiers"].as_number());

    const json::Value &s = root["settings"];
    if (s.is_object()) {
        if (s["match_threshold"].is_number())
            cfg.match_threshold = s["match_threshold"].as_number();
        if (s["trace_width"].is_number())
            cfg.trace_width = static_cast<int>(s["trace_width"].as_number());
        if (s["scroll_speed"].is_number())
            cfg.scroll_speed = s["scroll_speed"].as_number();
        if (s["scroll_invert"].is_bool())
            cfg.scroll_invert = s["scroll_invert"].as_bool();
        if (s["show_osd"].is_bool())
            cfg.show_osd = s["show_osd"].as_bool();
        if (s["trail_effect"].is_string())
            cfg.trail_effect = s["trail_effect"].as_string();
    }

    const json::Value &gestures = root["gestures"];
    if (gestures.is_array()) {
        for (const json::Value &g : gestures.as_array()) {
            GestureEntry e;
            if (g["name"].is_string())
                e.name = g["name"].as_string();
            if (g["type"].is_string()) {
                e.type = g["type"].as_string();
                if (g["argument"].is_string())
                    e.argument = g["argument"].as_string();
            } else {
                // Legacy format: separate key/text/command fields, priority
                // key > text > command. Fold into the typed model.
                auto str = [&](const char *k) {
                    return g[k].is_string() ? g[k].as_string() : std::string();
                };
                if (!str("key").empty()) {
                    e.type = "key";
                    e.argument = str("key");
                } else if (!str("text").empty()) {
                    e.type = "text";
                    e.argument = str("text");
                } else if (!str("command").empty()) {
                    e.type = "command";
                    e.argument = str("command");
                }
            }
            auto read_stroke = [](const json::Value &arr) {
                std::vector<Point> stroke;
                for (const json::Value &p : arr.as_array())
                    if (p.is_array() && p.as_array().size() >= 2)
                        stroke.push_back(
                            {p.as_array()[0].as_number(), p.as_array()[1].as_number()});
                return stroke;
            };
            // Current format: "strokes" = array of strokes (each an array of
            // [x,y]). Fall back to a legacy single "points" array.
            if (const json::Value &strokes = g["strokes"]; strokes.is_array()) {
                for (const json::Value &s : strokes.as_array())
                    if (s.is_array())
                        e.strokes.push_back(read_stroke(s));
            } else if (const json::Value &pts = g["points"]; pts.is_array()) {
                e.strokes.push_back(read_stroke(pts));
            }
            cfg.gestures.push_back(std::move(e));
        }
    }
    return cfg;
}

void GestureConfig::save(const std::string &path) const {
    json::Object root;
    root["trigger_button"] = json::Value(static_cast<int>(trigger_button));
    root["mode"] = json::Value(mode);
    root["trigger_modifiers"] = json::Value(static_cast<int>(trigger_modifiers));

    json::Object settings;
    settings["match_threshold"] = json::Value(match_threshold);
    settings["trace_width"] = json::Value(trace_width);
    settings["scroll_speed"] = json::Value(scroll_speed);
    settings["scroll_invert"] = json::Value(scroll_invert);
    settings["show_osd"] = json::Value(show_osd);
    settings["trail_effect"] = json::Value(trail_effect);
    root["settings"] = json::Value(std::move(settings));

    json::Array out_gestures;
    for (const GestureEntry &e : gestures) {
        json::Object o;
        o["name"] = json::Value(e.name);
        o["type"] = json::Value(e.type);
        o["argument"] = json::Value(e.argument);
        json::Array strokes;
        for (const std::vector<Point> &stroke : e.strokes) {
            json::Array pts;
            for (const Point &p : stroke) {
                json::Array xy;
                xy.push_back(json::Value(p.x));
                xy.push_back(json::Value(p.y));
                pts.push_back(json::Value(std::move(xy)));
            }
            strokes.push_back(json::Value(std::move(pts)));
        }
        o["strokes"] = json::Value(std::move(strokes));
        out_gestures.push_back(json::Value(std::move(o)));
    }
    root["gestures"] = json::Value(std::move(out_gestures));

    fs::path p(path);
    if (p.has_parent_path())
        fs::create_directories(p.parent_path());

    std::ofstream out(path, std::ios::trunc);
    if (!out)
        throw std::runtime_error("cannot write config: " + path);
    out << json::Value(std::move(root)).dump() << '\n';
}

} // namespace es
