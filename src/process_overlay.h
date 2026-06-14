#pragma once
#include "trace_overlay.h"

#include <cstdio>
#include <string>
#include <sys/types.h>

namespace es {

// A TraceOverlay backed by a separate process: the eswl-overlay GTK4 layer-shell
// binary. We fork/exec it once and stream line-based commands to its stdin:
//   B            begin a new stroke
//   P <x> <y>    append a point
//   E            end the stroke (clear)
// Keeping the renderer in its own process keeps the input engine free of any GUI
// toolkit and means an overlay crash can never take down input capture.
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
    void add(double x, double y) override;
    void end() override;

    // Sets the trail line width (px) on the renderer.
    void set_width(int px);

    // Sets the trail effect: 0 plain, 1 pixel, 2 sparkle.
    void set_effect(int effect);

    // Sets the completion fade-out duration (ms; 0 = instant).
    void set_fade_ms(int ms);

    // Flashes the matched gesture name on screen (OSD).
    void show_osd(const std::string &name);

private:
    std::FILE *pipe_ = nullptr; // write end of the overlay's stdin
    pid_t pid_ = -1;
};

} // namespace es
