#include "process_overlay.h"

#include <csignal>
#include <stdexcept>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

namespace es {

ProcessOverlay::ProcessOverlay(const std::string &binary_path, int screen_w, int screen_h) {
    int fds[2];
    if (::pipe(fds) != 0)
        throw std::runtime_error("overlay: pipe() failed");

    pid_ = ::fork();
    if (pid_ < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        throw std::runtime_error("overlay: fork() failed");
    }

    if (pid_ == 0) {
        // Child: wire the pipe's read end to stdin, then exec the renderer.
        ::dup2(fds[0], STDIN_FILENO);
        ::close(fds[0]);
        ::close(fds[1]);
        std::string screen = std::to_string(screen_w) + "x" + std::to_string(screen_h);
        ::execl(binary_path.c_str(), binary_path.c_str(), "--screen", screen.c_str(),
                static_cast<char *>(nullptr));
        ::_exit(127); // exec failed
    }

    // Parent: keep the write end, line-buffered so commands flush per newline.
    ::close(fds[0]);
    pipe_ = ::fdopen(fds[1], "w");
    if (!pipe_) {
        ::close(fds[1]);
        throw std::runtime_error("overlay: fdopen failed");
    }
    ::setvbuf(pipe_, nullptr, _IOLBF, 0);
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

// Writes never abort the daemon: SIGPIPE is ignored in main(), so a dead overlay
// just makes these fail with EPIPE, which we ignore.
void ProcessOverlay::begin() {
    if (pipe_)
        std::fputs("B\n", pipe_);
}

void ProcessOverlay::add(double x, double y) {
    if (pipe_)
        std::fprintf(pipe_, "P %.1f %.1f\n", x, y);
}

void ProcessOverlay::end() {
    if (pipe_)
        std::fputs("E\n", pipe_);
}

void ProcessOverlay::set_width(int px) {
    if (pipe_)
        std::fprintf(pipe_, "W %d\n", px);
}

void ProcessOverlay::show_osd(const std::string &name) {
    if (!pipe_)
        return;
    // Strip newlines so it stays a single command line.
    std::string clean;
    for (char c : name)
        if (c != '\n' && c != '\r')
            clean += c;
    std::fprintf(pipe_, "O %s\n", clean.c_str());
}

} // namespace es
