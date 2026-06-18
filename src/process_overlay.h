#pragma once
#include "trace_overlay.h"

#include <cstdio>
#include <map>
#include <string>
#include <sys/types.h>

namespace es {

// A TraceOverlay backed by a separate process: the eswl-overlay GTK4 layer-shell
// binary. We fork/exec it and stream line-based commands to its stdin:
//   B            begin a new stroke
//   P <x> <y>    append a point
//   E            end the stroke (clear)
// Keeping the renderer in its own process keeps the input engine free of any GUI
// toolkit and means an overlay crash can never take down input capture.
//
// The renderer is self-healing: if it dies (e.g. a compositor restart drops its
// Wayland connection, which shows up as a write failure on our pipe) it is
// re-forked on the next command, and the persistent settings (width, effect,
// colours, …) are replayed so the new renderer matches the daemon's current
// state. Without this a single KWin restart would silently kill the trail until
// the whole daemon was restarted.
class ProcessOverlay final : public TraceOverlay {
public:
    // binary_path: absolute path to the eswl-overlay executable.
    // screen_w/screen_h: the coordinate space points are sent in (the overlay
    // scales them to the real output size). Throws on spawn failure.
    ProcessOverlay(const std::string &binary_path, int screen_w, int screen_h);
    ~ProcessOverlay() override;

    ProcessOverlay(const ProcessOverlay &) = delete;
    ProcessOverlay &operator=(const ProcessOverlay &) = delete;

    void begin() override;
    void add(double x, double y, double pressure = -1.0) override;
    void end() override;
    void anchor_show(double x, double y) override;
    void anchor_hide() override;

    // Sets the trail line width (px) on the renderer.
    void set_width(int px);

    // Sets the trail effect: 0 plain, 1 glow, 2 sparkle.
    void set_effect(int effect);

    // Sets the trail/ring gradient endpoints (RGB 0..1): start -> end along the
    // stroke.
    void set_colors(double r0, double g0, double b0, double r1, double g1, double b1);

    // Sets the completion fade-out duration (ms; 0 = instant).
    void set_fade_ms(int ms);

    // Sets the touch anchor-ring ("armed" cue) radius in px.
    void set_anchor_radius(int px);

    // Sets the anchor-ring animation timing: grow-out and shrink/fade-out (ms).
    void set_anchor_timing(int grow_ms, int out_ms);

    // Pressure-sensitive pen trail width: enable + the width range (px) the
    // pen pressure (0..1) maps onto.
    void set_pressure(bool enabled, int min_px, int max_px);

    // Flashes the matched gesture name on screen (OSD).
    void show_osd(const std::string &name);

private:
    bool try_spawn();              // fork+exec eswl-overlay; sets pid_/pipe_; false on failure
    void ensure_child();           // respawn the renderer if it has died, replaying config
    void drop_pipe();              // close our write end; mark the child gone
    bool raw_write(const char *line);  // fputs to pipe_; drop_pipe() + false on write failure
    void send(const char *line);   // ensure_child + write, with one respawn-and-resend on failure
    void send_sticky(char key, const std::string &line); // remember + send a persistent setting
    void replay_config();          // resend every persistent setting to a freshly spawned child

    std::string  binary_path_;     // absolute path to eswl-overlay, kept for respawn
    int          screen_w_ = 0;
    int          screen_h_ = 0;
    std::FILE   *pipe_ = nullptr;  // write end of the overlay's stdin
    pid_t        pid_ = -1;
    std::map<char, std::string> sticky_; // latest value of each persistent setting (W/F/C/D/R/G/M)
};

} // namespace es
