// easystroke-wayland input-engine daemon (bring-up scaffold).
//
// Captures pointer input via libinput, recognizes strokes with the recycled
// core, and runs the bound action. Gestures are persisted in self-contained
// JSON (no Boost) and can be captured with --record.

#include "evdev_source.h"
#include "gesture_config.h"
#include "gesture_recognizer.h"
#include "keymap.h"
#include "libinput_source.h"
#include "process.h"
#include "process_overlay.h"
#include "uinput_injector.h"

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace es;

namespace {

std::atomic<bool> g_running{true};
void on_signal(int) { g_running = false; }

// Directory of our own executable, so we can find the sibling eswl-overlay
// binary whether running from the build tree or an install prefix.
std::string self_dir() {
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0)
        return ".";
    buf[n] = '\0';
    std::string path(buf);
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

// Captures one stroke for --record mode, then signals completion.
class RecorderSink final : public InputSink {
public:
    explicit RecorderSink(Button trigger) : trigger_(trigger) {}

    bool done() const { return done_; }
    const std::vector<Point> &points() const { return points_; }

    void on_button(Button button, bool pressed, Sample at) override {
        if (button != trigger_)
            return;
        if (pressed) {
            recording_ = true;
            buf_.clear();
            buf_.push_back({at.x, at.y});
            origin_ = {at.x, at.y};
            travel_ = 0.0;
        } else if (recording_) {
            recording_ = false;
            if (travel_ >= GestureRecognizer::kGestureMinTravel && buf_.size() > 2) {
                points_ = buf_;
                done_ = true;
            } else {
                std::printf("stroke too short (travel %.0fpx, %zu pts) — try again\n", travel_,
                            buf_.size());
            }
        }
    }
    void on_motion(Sample at, double, double) override {
        if (!recording_)
            return;
        buf_.push_back({at.x, at.y});
        double d = std::hypot(at.x - origin_.x, at.y - origin_.y);
        if (d > travel_)
            travel_ = d;
    }
    void on_scroll(double, double, Sample) override {}

private:
    Button trigger_;
    bool recording_ = false;
    bool done_ = false;
    std::vector<Point> buf_;
    std::vector<Point> points_;
    Point origin_;
    double travel_ = 0.0;
};

// Pumps events until the signal flag clears or `keep_going` returns false.
template <typename Pred> void run_loop(InputSource &source, Pred keep_going) {
    pollfd pfd;
    pfd.fd = source.fd();
    pfd.events = POLLIN;
    pfd.revents = 0;
    while (g_running && keep_going()) {
        int r = ::poll(&pfd, 1, 200);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            std::perror("poll");
            break;
        }
        if (r > 0 && (pfd.revents & POLLIN))
            source.dispatch();
    }
}

[[noreturn]] void usage(const char *me, int code) {
    std::printf("usage: %s [options]\n", me);
    std::printf("  --config PATH    config file (default: %s)\n",
                GestureConfig::default_path().c_str());
    std::printf("  --record NAME    capture one stroke, save it as NAME, and exit\n");
    std::printf("  --button N       override trigger button (1 left, 2 mid, 3 right, 8/9 thumb,\n");
    std::printf("                   10 pen-tip, 11 pen-button)\n");
    std::printf("  --screen WxH     screen size for pointer tracking (default 1920x1080)\n");
    std::printf("  --threshold T    match score floor 0..1 (default 0.6; lower = more lenient)\n");
    std::printf("  --overlay        draw the live stroke trail (gtk4 layer-shell overlay)\n");
    std::printf("  --grab           grab mice (EVIOCGRAB) to suppress the trigger button;\n");
    std::printf("                   without it, capture is monitor-only and the button also clicks\n");
    std::exit(code);
}

} // namespace

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    int screen_w = 1920;
    int screen_h = 1080;
    int button_override = -1;
    double threshold = 0.6; // match score floor; pen strokes peak lower than mouse
    bool grab = false;
    bool overlay = false;
    std::string config_path = GestureConfig::default_path();
    std::string record_name;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help") {
            usage(argv[0], 0);
        } else if (a == "--screen" && i + 1 < argc) {
            std::sscanf(argv[++i], "%dx%d", &screen_w, &screen_h);
        } else if (a == "--button" && i + 1 < argc) {
            button_override = std::atoi(argv[++i]);
        } else if (a == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        } else if (a == "--record" && i + 1 < argc) {
            record_name = argv[++i];
        } else if (a == "--threshold" && i + 1 < argc) {
            threshold = std::atof(argv[++i]);
        } else if (a == "--grab") {
            grab = true;
        } else if (a == "--overlay") {
            overlay = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            usage(argv[0], 2);
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGCHLD, SIG_IGN); // auto-reap action commands
    std::signal(SIGPIPE, SIG_IGN); // a dead overlay must not kill the daemon

    GestureConfig cfg;
    try {
        cfg = GestureConfig::load(config_path);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: bad config %s: %s\n", config_path.c_str(), e.what());
        return 1;
    }
    Button trigger = button_override > 0 ? static_cast<Button>(button_override) : cfg.trigger_button;

    // --- Record mode ----------------------------------------------------
    if (!record_name.empty()) {
        RecorderSink rec(trigger);
        std::unique_ptr<LibinputSource> source;
        try {
            source = std::make_unique<LibinputSource>(rec, screen_w, screen_h);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "error: %s\n", e.what());
            return 1;
        }
        std::printf("Recording '%s': hold button %u, draw the stroke, then release.\n",
                    record_name.c_str(), trigger);
        run_loop(*source, [&] { return !rec.done(); });
        if (!rec.done()) {
            std::printf("aborted; nothing saved.\n");
            return 1;
        }
        cfg.trigger_button = trigger;
        GestureEntry *existing = nullptr;
        for (GestureEntry &e : cfg.gestures)
            if (e.name == record_name) {
                existing = &e;
                break;
            }
        if (existing) {
            // Recording an existing name appends another example for sturdier
            // matching, rather than creating a duplicate gesture.
            existing->strokes.push_back(rec.points());
            std::printf("added example #%zu to gesture '%s' (%zu points) in %s\n",
                        existing->strokes.size(), record_name.c_str(), rec.points().size(),
                        config_path.c_str());
        } else {
            GestureEntry e;
            e.name = record_name;
            e.strokes.push_back(rec.points());
            cfg.gestures.push_back(std::move(e));
            std::printf("saved gesture '%s' (%zu points) to %s\n", record_name.c_str(),
                        rec.points().size(), config_path.c_str());
            std::printf("set its action by editing the \"key\"/\"text\"/\"command\" field, or "
                        "record the same name again to add another example.\n");
        }
        cfg.save(config_path);
        return 0;
    }

    // --- Normal mode ----------------------------------------------------
    std::unique_ptr<UinputInjector> injector;
    try {
        injector = std::make_unique<UinputInjector>();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "warning: uinput unavailable: %s\n", e.what());
    }

    GestureRecognizer recognizer(trigger, threshold);

    Keymap keymap;
    if (!keymap.ok())
        std::fprintf(stderr, "warning: no XKB keymap; key/text actions disabled\n");

    InputInjector *inj = injector.get();
    for (const GestureEntry &g : cfg.gestures) {
        std::vector<Gesture> strokes;
        for (const std::vector<Point> &pts : g.strokes) {
            Gesture st = Gesture::from_points(pts);
            if (st.valid())
                strokes.push_back(std::move(st));
        }
        std::string name = g.name;
        std::function<void()> action;

        if (!g.key.empty()) {
            KeyStroke ks;
            if (inj && keymap.from_combo(g.key, ks))
                action = [inj, ks] { send_keystroke(*inj, ks); };
            else
                std::fprintf(stderr, "warning: gesture '%s': cannot bind key '%s'\n",
                             name.c_str(), g.key.c_str());
        } else if (!g.text.empty()) {
            std::string text = g.text;
            if (inj && keymap.ok())
                action = [inj, &keymap, text] { type_text(keymap, *inj, text); };
            else
                std::fprintf(stderr, "warning: gesture '%s': cannot bind text action\n",
                             name.c_str());
        } else if (!g.command.empty()) {
            std::string command = g.command;
            action = [command] { run_command(command); };
        }

        if (!action)
            action = [name] { std::printf("  (gesture '%s' has no usable action)\n", name.c_str()); };
        recognizer.add_binding({name, std::move(strokes), std::move(action)});
    }
    recognizer.set_reporter([](const Recognition &r) {
        if (r.matched)
            std::printf("[gesture] matched '%s' (score %.2f)\n", r.name.c_str(), r.score);
        else if (r.points > 2)
            std::printf("[gesture] no match (best score %.2f)\n", r.score);
    });

    std::unique_ptr<ProcessOverlay> overlay_proc;
    if (overlay) {
        try {
            overlay_proc =
                std::make_unique<ProcessOverlay>(self_dir() + "/eswl-overlay", screen_w, screen_h);
            recognizer.set_overlay(overlay_proc.get());
        } catch (const std::exception &e) {
            std::fprintf(stderr, "warning: overlay unavailable: %s\n", e.what());
        }
    }

    std::unique_ptr<InputSource> source;
    try {
        if (grab) {
            if (!inj)
                throw std::runtime_error("--grab needs uinput (forwarding requires injection)");
            source = std::make_unique<EvdevSource>(recognizer, *inj, trigger, screen_w, screen_h);
        } else {
            source = std::make_unique<LibinputSource>(recognizer, screen_w, screen_h);
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        std::fprintf(stderr, "       (run in the 'input' group, or as the privileged daemon)\n");
        return 1;
    }

    if (grab)
        std::printf("GRAB MODE: mice are exclusively grabbed; the keyboard stays free "
                    "(Ctrl-C always works).\n         The trigger button is suppressed for "
                    "gestures and replayed for plain clicks.\n");
    std::printf("easystroke-wayland running: %zu gesture(s), trigger button %u, screen %dx%d.\n",
                cfg.gestures.size(), trigger, screen_w, screen_h);
    if (cfg.gestures.empty())
        std::printf("no gestures yet — record one with: %s --record NAME\n", argv[0]);
    run_loop(*source, [] { return true; });

    std::printf("\nshutting down.\n");
    return 0;
}
