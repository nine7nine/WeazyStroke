#pragma once
#include <string>

namespace es {

// Runs a shell command asynchronously (fork + exec "/bin/sh -c cmd") in its own
// session, without blocking the caller. No-op for an empty command. Callers
// must ignore or reap SIGCHLD to avoid zombies.
void run_command(const std::string &cmd);

} // namespace es
