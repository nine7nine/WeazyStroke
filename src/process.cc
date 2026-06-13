#include "process.h"

#include <sys/wait.h>
#include <unistd.h>

namespace es {

void run_command(const std::string &cmd) {
    if (cmd.empty())
        return;
    pid_t pid = ::fork();
    if (pid == 0) {
        // Child: detach from the daemon's session and exec a shell.
        ::setsid();
        ::execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char *>(nullptr));
        ::_exit(127);
    }
    // Parent returns immediately; SIGCHLD is ignored by the daemon so the child
    // is auto-reaped.
}

} // namespace es
