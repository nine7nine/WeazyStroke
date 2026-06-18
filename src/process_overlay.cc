#include "process_overlay.h"

#include <csignal>
#include <cstdio>
#include <stdexcept>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

namespace es {

ProcessOverlay::ProcessOverlay(const std::string &binary_path, int screen_w, int screen_h)
    : binary_path_(binary_path), screen_w_(screen_w), screen_h_(screen_h) {
    if (!try_spawn())
        throw std::runtime_error("overlay: spawn failed");
}

ProcessOverlay::~ProcessOverlay() {
    if (pipe_)
        std::fclose(pipe_); // EOF on the overlay's stdin -> it exits its main loop
    if (pid_ > 0) {
        ::kill(pid_, SIGTERM);
        int status = 0;
        ::waitpid(pid_, &status, 0); // harmless ECHILD if SIGCHLD is auto-reaping
    }
}

// Fork the eswl-overlay renderer, wiring a fresh pipe to its stdin. Returns false
// on any failure (caller decides whether that is fatal). Never throws, so it is
// safe to call from the input path for an on-demand respawn.
bool ProcessOverlay::try_spawn() {
    int fds[2];
    if (::pipe(fds) != 0)
        return false;

    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return false;
    }

    if (pid == 0) {
        // Child: wire the pipe's read end to stdin, then exec the renderer.
        ::dup2(fds[0], STDIN_FILENO);
        ::close(fds[0]);
        ::close(fds[1]);
        std::string screen = std::to_string(screen_w_) + "x" + std::to_string(screen_h_);
        ::execl(binary_path_.c_str(), binary_path_.c_str(), "--screen", screen.c_str(),
                static_cast<char *>(nullptr));
        ::_exit(127); // exec failed
    }

    // Parent: keep the write end, line-buffered so commands flush per newline.
    ::close(fds[0]);
    std::FILE *pipe = ::fdopen(fds[1], "w");
    if (!pipe) {
        ::close(fds[1]);
        return false;
    }
    ::setvbuf(pipe, nullptr, _IOLBF, 0);
    pid_ = pid;
    pipe_ = pipe;
    return true;
}

// Close our write end and forget the child. SIGCHLD is SIG_IGN in the daemon, so
// the dead child is auto-reaped — no waitpid (and so no zombie) needed here.
void ProcessOverlay::drop_pipe() {
    if (pipe_) {
        std::fclose(pipe_);
        pipe_ = nullptr;
    }
    pid_ = -1;
}

// Re-fork the renderer if it has gone away, then replay every persistent setting
// so it matches the daemon's current state.
void ProcessOverlay::ensure_child() {
    if (pipe_)
        return;
    if (try_spawn())
        replay_config();
}

// Low-level write. The trail commands all end in '\n' and the pipe is line
// buffered, so each call flushes immediately; a dead renderer (closed read end)
// makes fputs fail with EOF (SIGPIPE is ignored daemon-wide). On failure we tear
// the pipe down so the next send() respawns.
bool ProcessOverlay::raw_write(const char *line) {
    if (!pipe_)
        return false;
    if (std::fputs(line, pipe_) == EOF) {
        drop_pipe();
        return false;
    }
    return true;
}

void ProcessOverlay::send(const char *line) {
    if (!pipe_)
        ensure_child(); // renderer died while idle -> bring it back first
    if (raw_write(line))
        return;
    // The write failed (the child died as we wrote, or a respawn failed): try one
    // respawn + resend so this command is not silently lost.
    ensure_child();
    raw_write(line);
}

// Persistent settings are remembered (latest value per command letter) so they
// can be replayed to a renderer we respawn later.
void ProcessOverlay::send_sticky(char key, const std::string &line) {
    sticky_[key] = line;
    send(line.c_str());
}

void ProcessOverlay::replay_config() {
    // Stable order (std::map by command letter); each setting is independent so
    // order does not matter. raw_write drops the pipe on failure, ending the loop.
    for (const auto &kv : sticky_) {
        if (!raw_write(kv.second.c_str()))
            return; // the fresh child is already gone; next send() will retry
    }
}

// ---- transient stroke/cue commands (not replayed on respawn) ----

void ProcessOverlay::begin() {
    send("B\n");
}

void ProcessOverlay::add(double x, double y, double pressure) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "P %.1f %.1f %.3f\n", x, y, pressure);
    send(buf);
}

void ProcessOverlay::end() {
    send("E\n");
}

void ProcessOverlay::anchor_show(double x, double y) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "A %.1f %.1f\n", x, y);
    send(buf);
}

void ProcessOverlay::anchor_hide() {
    send("a\n");
}

void ProcessOverlay::show_osd(const std::string &name) {
    // Strip newlines so it stays a single command line.
    std::string clean = "O ";
    for (char c : name)
        if (c != '\n' && c != '\r')
            clean += c;
    clean += '\n';
    send(clean.c_str());
}

// ---- persistent settings (remembered + replayed after a respawn) ----

void ProcessOverlay::set_width(int px) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "W %d\n", px);
    send_sticky('W', buf);
}

void ProcessOverlay::set_effect(int effect) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "F %d\n", effect);
    send_sticky('F', buf);
}

void ProcessOverlay::set_colors(double r0, double g0, double b0, double r1, double g1, double b1) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "C %.3f %.3f %.3f %.3f %.3f %.3f\n", r0, g0, b0, r1, g1, b1);
    send_sticky('C', buf);
}

void ProcessOverlay::set_fade_ms(int ms) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "D %d\n", ms);
    send_sticky('D', buf);
}

void ProcessOverlay::set_anchor_radius(int px) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "R %d\n", px);
    send_sticky('R', buf);
}

void ProcessOverlay::set_anchor_timing(int grow_ms, int out_ms) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "G %d %d\n", grow_ms, out_ms);
    send_sticky('G', buf);
}

void ProcessOverlay::set_pressure(bool enabled, int min_px, int max_px) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "M %d %d %d\n", enabled ? 1 : 0, min_px, max_px);
    send_sticky('M', buf);
}

} // namespace es
