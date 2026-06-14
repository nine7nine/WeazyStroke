#include "process_tray.h"

#include <csignal>
#include <stdexcept>

#include <sys/wait.h>
#include <unistd.h>

namespace es {

ProcessTray::ProcessTray(const std::string &binary_path) {
    int fds[2];
    if (::pipe(fds) != 0)
        throw std::runtime_error("tray: pipe() failed");

    pid_ = ::fork();
    if (pid_ < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        throw std::runtime_error("tray: fork() failed");
    }

    if (pid_ == 0) {
        // Child: wire the pipe's read end to stdin, then exec the tray helper.
        ::dup2(fds[0], STDIN_FILENO);
        ::close(fds[0]);
        ::close(fds[1]);
        ::execl(binary_path.c_str(), binary_path.c_str(), static_cast<char *>(nullptr));
        ::_exit(127); // exec failed
    }

    ::close(fds[0]);
    pipe_ = ::fdopen(fds[1], "w");
    if (!pipe_) {
        ::close(fds[1]);
        throw std::runtime_error("tray: fdopen failed");
    }
    ::setvbuf(pipe_, nullptr, _IOLBF, 0);
}

ProcessTray::~ProcessTray() {
    if (pipe_)
        std::fclose(pipe_); // EOF on the tray's stdin -> it quits its main loop
    if (pid_ > 0) {
        ::kill(pid_, SIGTERM);
        int status = 0;
        ::waitpid(pid_, &status, 0); // harmless ECHILD if SIGCHLD is auto-reaping
    }
}

void ProcessTray::set_enabled(bool enabled) {
    if (pipe_)
        std::fputs(enabled ? "E\n" : "D\n", pipe_);
}

} // namespace es
