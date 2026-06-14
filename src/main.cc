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
#include "process_tray.h"
#include "uinput_injector.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <unistd.h>

#include <cstdint>
#include <ctime>

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

// SIGHUP asks the daemon to reload its gesture config (the config GUI raises it
// after saving), so edits take effect without a restart.
std::atomic<bool> g_reload{false};
void on_reload(int) { g_reload = true; }

// When disabled, recognized gestures don't run their actions (except the Misc
// toggle itself, so you can always re-enable). Toggled by a Misc gesture or
// SIGUSR1 (raised by the GUI's pause button).
std::atomic<bool> g_disabled{false};
void on_toggle(int) { g_disabled = !g_disabled; }

// Single-instance lock so only one daemon runs (a second would double every
// action). Returns the held fd (kept open for the process lifetime), -1 if
// another instance already holds it, or -2 if the lock couldn't be created.
int acquire_singleton_lock() {
    const char *rt = std::getenv("XDG_RUNTIME_DIR");
    std::string path = std::string(rt && *rt ? rt : "/tmp") + "/weazystroke.lock";
    int fd = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0)
        return -2;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

uint32_t monotonic_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint32_t>(ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull);
}

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

// Parse a scroll argument ("up", "down 3", "left", "right 2") into wheel detents
// (dx horizontal, dy vertical; +y scrolls up, +x scrolls right). Default count 3.
bool parse_scroll(const std::string &arg, int &dx, int &dy) {
    char dir[16] = {0};
    int count = 3;
    if (std::sscanf(arg.c_str(), "%15s %d", dir, &count) < 1)
        return false;
    if (count <= 0)
        count = 3;
    std::string d = dir;
    dx = dy = 0;
    if (d == "up")
        dy = count;
    else if (d == "down")
        dy = -count;
    else if (d == "left")
        dx = -count;
    else if (d == "right")
        dx = count;
    else
        return false;
    return true;
}

// Screen-edge name -> TouchEdge (anything unrecognized, incl. "none", = off).
TouchEdge parse_edge(const std::string &e) {
    if (e == "left")
        return TouchEdge::Left;
    if (e == "right")
        return TouchEdge::Right;
    if (e == "top")
        return TouchEdge::Top;
    if (e == "bottom")
        return TouchEdge::Bottom;
    return TouchEdge::None;
}

// Trail effect name -> overlay effect id.
int effect_id(const std::string &e) {
    if (e == "glow")
        return 1;
    if (e == "sparkle")
        return 2;
    return 0;
}

// Directory portion of a path (with trailing slash), or "" if none.
std::string config_dir(const std::string &path) {
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

// Append one recognition result to the history log (read by the GUI's History
// tab for tuning). One JSON object per line.
void append_history(const std::string &path, const Recognition &r) {
    std::FILE *f = std::fopen(path.c_str(), "a");
    if (!f)
        return;
    std::time_t now = std::time(nullptr);
    char ts[16] = {0};
    std::strftime(ts, sizeof ts, "%H:%M:%S", std::localtime(&now));
    std::string name;
    for (char c : r.name) { // minimal JSON string escaping
        if (c == '"' || c == '\\')
            name += '\\';
        name += c;
    }
    std::fprintf(f, "{\"t\":\"%s\",\"matched\":%s,\"name\":\"%s\",\"score\":%.3f,\"points\":%d}\n",
                 ts, r.matched ? "true" : "false", name.c_str(), r.score, r.points);
    std::fclose(f);
}

// Build the recognizer's gesture bindings from a config. Called at startup and
// on SIGHUP reload. `keymap` is captured by reference in key/text actions, so it
// must outlive the recognizer (it does: both live for the whole run).
void build_bindings(GestureRecognizer &recognizer, const GestureConfig &cfg, InputInjector *inj,
                    Keymap &keymap) {
    for (const GestureEntry &g : cfg.gestures) {
        std::vector<Gesture> strokes;
        for (const std::vector<Point> &pts : g.strokes) {
            Gesture st = Gesture::from_points(pts);
            if (st.valid())
                strokes.push_back(std::move(st));
        }
        std::string name = g.name;
        const std::string &arg = g.argument;
        std::function<void()> action;

        if (g.type == "command") {
            std::string command = arg;
            action = [command] { run_command(command); };
        } else if (g.type == "key") {
            KeyStroke ks;
            if (inj && keymap.from_combo(arg, ks))
                action = [inj, ks] { send_keystroke(*inj, ks); };
            else
                std::fprintf(stderr, "warning: gesture '%s': cannot bind key '%s'\n", name.c_str(),
                             arg.c_str());
        } else if (g.type == "text") {
            std::string text = arg;
            if (inj && keymap.ok())
                action = [inj, &keymap, text] { type_text(keymap, *inj, text); };
            else
                std::fprintf(stderr, "warning: gesture '%s': cannot bind text action\n",
                             name.c_str());
        } else if (g.type == "button") {
            int n = std::atoi(arg.c_str());
            if (inj && n > 0) {
                Button b = static_cast<Button>(n);
                action = [inj, b] { inj->click(b); };
            } else
                std::fprintf(stderr, "warning: gesture '%s': bad button '%s'\n", name.c_str(),
                             arg.c_str());
        } else if (g.type == "scroll") {
            int dx = 0, dy = 0;
            if (inj && parse_scroll(arg, dx, dy)) {
                double sp = cfg.scroll_speed > 0 ? cfg.scroll_speed : 1.0;
                int sgn = cfg.scroll_invert ? -1 : 1;
                int sdx = static_cast<int>(std::lround(dx * sp)) * sgn;
                int sdy = static_cast<int>(std::lround(dy * sp)) * sgn;
                action = [inj, sdx, sdy] {
                    inj->scroll(sdx, sdy);
                    inj->flush();
                };
            } else
                std::fprintf(stderr,
                             "warning: gesture '%s': bad scroll '%s' (use up/down/left/right "
                             "[count])\n",
                             name.c_str(), arg.c_str());
        } else if (g.type == "ignore") {
            action = [] {}; // recognized, but deliberately does nothing
        } else if (g.type == "misc") {
            if (arg == "disable")
                action = [] { g_disabled = !g_disabled; };
            // future misc subtypes: "settings", "unminimize", ...
        }

        if (!action)
            action = [name] { std::printf("  (gesture '%s' has no usable action)\n", name.c_str()); };

        // Honor the global enable/disable toggle: when disabled, every action is
        // suppressed except the Misc toggle itself (so you can re-enable).
        if (g.type != "misc")
            action = [a = std::move(action)] {
                if (!g_disabled)
                    a();
            };
        recognizer.add_binding({name, std::move(strokes), std::move(action)});
    }
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
        int r = ::poll(&pfd, 1, 50); // short timeout so keep_going() ticks often
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
    std::printf("  --touch-edge E   two-finger touch: edge the anchor finger starts from\n");
    std::printf("                   (none|left|right|top|bottom; overrides the config)\n");
    std::printf("  --overlay        draw the live stroke trail (gtk4 layer-shell overlay)\n");
    std::printf("  --tray           show a system-tray icon (enable/disable, prefs, quit)\n");
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
    double cli_threshold = -1.0; // <0 => take match_threshold from the config
    std::string touch_edge_override; // empty => take touch_edge from the config
    bool grab = false;
    bool overlay = false;
    bool tray = false;
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
            cli_threshold = std::atof(argv[++i]);
        } else if (a == "--touch-edge" && i + 1 < argc) {
            touch_edge_override = argv[++i];
        } else if (a == "--grab") {
            grab = true;
        } else if (a == "--overlay") {
            overlay = true;
        } else if (a == "--tray") {
            tray = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", a.c_str());
            usage(argv[0], 2);
        }
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGHUP, on_reload);  // config GUI raises this after saving
    std::signal(SIGUSR1, on_toggle); // GUI pause button toggles enable/disable
    std::signal(SIGCHLD, SIG_IGN);   // auto-reap action commands
    std::signal(SIGPIPE, SIG_IGN);   // a dead overlay must not kill the daemon

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
            std::printf("set its action by editing \"type\"+\"argument\" (command/key/text/button/"
                        "scroll/ignore), or record the same name again to add another example.\n");
        }
        cfg.save(config_path);
        return 0;
    }

    // --- Normal mode ----------------------------------------------------
    int lock_fd = acquire_singleton_lock();
    if (lock_fd == -1) {
        std::fprintf(stderr, "another WeazyStroke daemon is already running; exiting.\n");
        return 0;
    }
    (void)lock_fd; // held for the process lifetime

    std::unique_ptr<UinputInjector> injector;
    try {
        injector = std::make_unique<UinputInjector>();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "warning: uinput unavailable: %s\n", e.what());
    }

    double threshold = cli_threshold >= 0 ? cli_threshold : cfg.match_threshold;
    GestureRecognizer recognizer(trigger, threshold);
    recognizer.set_required_modifiers(cfg.trigger_modifiers);
    recognizer.set_gate_button(cfg.gate_button); // pen "tip + side button" chord
    recognizer.set_debounce(trigger == 10 ? 120 : 0); // 10 = pen tip (BTN_TOUCH), debounce chatter
    TouchEdge touch_edge =
        parse_edge(!touch_edge_override.empty() ? touch_edge_override : cfg.touch_edge);
    recognizer.configure_touch(touch_edge, screen_w, screen_h, cfg.touch_band);
    recognizer.set_touch_cue(cfg.touch_cue);

    Keymap keymap;
    if (!keymap.ok())
        std::fprintf(stderr, "warning: no XKB keymap; key/text actions disabled\n");

    InputInjector *inj = injector.get();
    build_bindings(recognizer, cfg, inj, keymap);

    std::unique_ptr<ProcessOverlay> overlay_proc;
    if (overlay) {
        try {
            overlay_proc =
                std::make_unique<ProcessOverlay>(self_dir() + "/eswl-overlay", screen_w, screen_h);
            overlay_proc->set_width(cfg.trace_width);
            overlay_proc->set_effect(effect_id(cfg.trail_effect));
            overlay_proc->set_fade_ms(cfg.trail_fade_ms);
            overlay_proc->set_anchor_radius(cfg.touch_ring);
            overlay_proc->set_anchor_timing(cfg.touch_grow_ms, cfg.touch_out_ms);
            recognizer.set_overlay(overlay_proc.get());
        } catch (const std::exception &e) {
            std::fprintf(stderr, "warning: overlay unavailable: %s\n", e.what());
        }
    }

    std::string history_path = config_dir(config_path) + "history.jsonl";
    ProcessOverlay *op = overlay_proc.get();
    recognizer.set_reporter([history_path, op, &cfg](const Recognition &r) {
        if (r.matched)
            std::printf("[gesture] matched '%s' (score %.2f)\n", r.name.c_str(), r.score);
        else if (r.points > 2)
            std::printf("[gesture] no match (best score %.2f)\n", r.score);
        if (r.points > 2)
            append_history(history_path, r);
        if (r.matched && op && cfg.show_osd)
            op->show_osd(r.name);
    });

    std::unique_ptr<ProcessTray> tray_proc;
    if (tray) {
        try {
            tray_proc = std::make_unique<ProcessTray>(self_dir() + "/eswl-tray");
            tray_proc->set_enabled(!g_disabled);
        } catch (const std::exception &e) {
            std::fprintf(stderr, "warning: tray unavailable: %s\n", e.what());
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
    if (touch_edge != TouchEdge::None)
        std::printf("two-finger touch: anchor at the %s edge (%dpx band), second finger draws.\n",
                    (!touch_edge_override.empty() ? touch_edge_override : cfg.touch_edge).c_str(),
                    cfg.touch_band);
    if (cfg.gestures.empty())
        std::printf("no gestures yet — record one with: %s --record NAME\n", argv[0]);
    bool last_disabled = g_disabled.load();
    run_loop(*source, [&] {
        recognizer.tick(monotonic_ms()); // finalize debounced (pen-tip) releases
        // Mirror enable/disable changes (gesture, GUI, or tray-raised SIGUSR1)
        // to the tray so its checkmark/tooltip stay in sync.
        if (bool d = g_disabled.load(); tray_proc && d != last_disabled) {
            last_disabled = d;
            tray_proc->set_enabled(!d);
        }
        if (g_reload.exchange(false)) {
            try {
                cfg = GestureConfig::load(config_path);
                recognizer.clear_bindings();
                build_bindings(recognizer, cfg, inj, keymap);
                recognizer.set_threshold(cli_threshold >= 0 ? cli_threshold : cfg.match_threshold);
                recognizer.set_required_modifiers(cfg.trigger_modifiers);
                // Trigger / gate / debounce can change in the GUI; apply live.
                Button ntrig = button_override > 0 ? static_cast<Button>(button_override)
                                                   : cfg.trigger_button;
                recognizer.set_trigger(ntrig);
                recognizer.set_gate_button(cfg.gate_button);
                recognizer.set_debounce(ntrig == 10 ? 120 : 0);
                recognizer.configure_touch(
                    parse_edge(!touch_edge_override.empty() ? touch_edge_override : cfg.touch_edge),
                    screen_w, screen_h, cfg.touch_band);
                recognizer.set_touch_cue(cfg.touch_cue);
                if (overlay_proc) {
                    overlay_proc->set_width(cfg.trace_width);
                    overlay_proc->set_effect(effect_id(cfg.trail_effect));
                    overlay_proc->set_fade_ms(cfg.trail_fade_ms);
                    overlay_proc->set_anchor_radius(cfg.touch_ring);
                    overlay_proc->set_anchor_timing(cfg.touch_grow_ms, cfg.touch_out_ms);
                }
                std::printf("[reload] config reloaded: %zu gesture(s), threshold %.2f\n",
                            cfg.gestures.size(),
                            cli_threshold >= 0 ? cli_threshold : cfg.match_threshold);
            } catch (const std::exception &e) {
                std::fprintf(stderr, "reload failed: %s\n", e.what());
            }
        }
        return true;
    });

    std::printf("\nshutting down.\n");
    return 0;
}
