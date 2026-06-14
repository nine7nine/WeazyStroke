#pragma once

#include <cstdio>
#include <string>

#include <sys/types.h>

namespace es {

// Spawns and feeds the system-tray helper (eswl-tray) over a pipe, mirroring
// ProcessOverlay. The daemon pushes its enable/disable state down; the tray
// pushes user actions back as signals to the daemon (its parent).
class ProcessTray {
public:
    explicit ProcessTray(const std::string &binary_path);
    ~ProcessTray();
    ProcessTray(const ProcessTray &) = delete;
    ProcessTray &operator=(const ProcessTray &) = delete;

    // Tell the tray the daemon is enabled (true) or paused (false).
    void set_enabled(bool enabled);

private:
    std::FILE *pipe_ = nullptr;
    pid_t pid_ = -1;
};

} // namespace es
