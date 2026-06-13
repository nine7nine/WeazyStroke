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

    const json::Value &gestures = root["gestures"];
    if (gestures.is_array()) {
        for (const json::Value &g : gestures.as_array()) {
            GestureEntry e;
            if (g["name"].is_string())
                e.name = g["name"].as_string();
            if (g["key"].is_string())
                e.key = g["key"].as_string();
            if (g["text"].is_string())
                e.text = g["text"].as_string();
            if (g["command"].is_string())
                e.command = g["command"].as_string();
            const json::Value &pts = g["points"];
            if (pts.is_array()) {
                for (const json::Value &p : pts.as_array()) {
                    if (p.is_array() && p.as_array().size() >= 2)
                        e.points.push_back({p.as_array()[0].as_number(),
                                            p.as_array()[1].as_number()});
                }
            }
            cfg.gestures.push_back(std::move(e));
        }
    }
    return cfg;
}

void GestureConfig::save(const std::string &path) const {
    json::Object root;
    root["trigger_button"] = json::Value(static_cast<int>(trigger_button));

    json::Array out_gestures;
    for (const GestureEntry &e : gestures) {
        json::Object o;
        o["name"] = json::Value(e.name);
        o["key"] = json::Value(e.key);
        o["text"] = json::Value(e.text);
        o["command"] = json::Value(e.command);
        json::Array pts;
        for (const Point &p : e.points) {
            json::Array xy;
            xy.push_back(json::Value(p.x));
            xy.push_back(json::Value(p.y));
            pts.push_back(json::Value(std::move(xy)));
        }
        o["points"] = json::Value(std::move(pts));
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
