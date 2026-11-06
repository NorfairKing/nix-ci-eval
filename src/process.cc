#include "process.hh"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <stdexcept>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace nixcieval {

namespace {

// Prepare a forked child for exec and replace it with the program. Never
// returns.
[[noreturn]] void execChild(pid_t parentPid, const std::string & program,
                            const std::vector<std::string> & args, int stdoutFd,
                            int stderrFd) {
    // Die with the coordinator, like an evaluation worker does. The getppid
    // check closes the window where the parent died before prctl was reached.
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != parentPid) {
        _Exit(127);
    }
    // The coordinator runs with Nix's signal mask and ignores SIGPIPE. Both
    // survive exec, and neither is something git or iets expects.
    sigset_t empty;
    sigemptyset(&empty);
    pthread_sigmask(SIG_SETMASK, &empty, nullptr);
    signal(SIGPIPE, SIG_DFL);

    int devNull = open("/dev/null", O_RDONLY);
    if (devNull < 0 || dup2(devNull, STDIN_FILENO) < 0 ||
        dup2(stdoutFd, STDOUT_FILENO) < 0 ||
        dup2(stderrFd, STDERR_FILENO) < 0) {
        _Exit(127);
    }
    // Everything else the coordinator holds (worker pipes, the output stream)
    // stays out of the program.
    closefrom(STDERR_FILENO + 1);

    std::vector<char *> argv;
    argv.push_back(const_cast<char *>(program.c_str()));
    for (const auto & arg : args) {
        argv.push_back(const_cast<char *>(arg.c_str()));
    }
    argv.push_back(nullptr);
    execvp(program.c_str(), argv.data());
    _Exit(127);
}

pid_t forkOrThrow() {
    pid_t pid = fork();
    if (pid < 0) {
        throw std::runtime_error(std::string("fork failed: ") +
                                 std::strerror(errno));
    }
    return pid;
}

} // namespace

bool waitSucceeded(pid_t pid) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return false;
        }
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool runProgram(const std::string & program,
                const std::vector<std::string> & args,
                std::string * capturedStdout) {
    int pipeFds[2] = {-1, -1};
    if (capturedStdout != nullptr && pipe(pipeFds) != 0) {
        throw std::runtime_error(std::string("pipe failed: ") +
                                 std::strerror(errno));
    }
    pid_t parentPid = getpid();
    pid_t pid = forkOrThrow();
    if (pid == 0) {
        int out = capturedStdout != nullptr ? pipeFds[1] : STDERR_FILENO;
        execChild(parentPid, program, args, out, STDERR_FILENO);
    }
    if (capturedStdout != nullptr) {
        close(pipeFds[1]);
        char buffer[4096];
        while (true) {
            ssize_t got = read(pipeFds[0], buffer, sizeof(buffer));
            if (got < 0 && errno == EINTR) {
                continue;
            }
            if (got <= 0) {
                break;
            }
            capturedStdout->append(buffer, static_cast<std::size_t>(got));
        }
        close(pipeFds[0]);
    }
    return waitSucceeded(pid);
}

Spawned spawnProgram(const std::string & program,
                     const std::vector<std::string> & args, int stderrFd) {
    int pipeFds[2];
    if (pipe(pipeFds) != 0) {
        throw std::runtime_error(std::string("pipe failed: ") +
                                 std::strerror(errno));
    }
    pid_t parentPid = getpid();
    pid_t pid = -1;
    try {
        pid = forkOrThrow();
    } catch (...) {
        close(pipeFds[0]);
        close(pipeFds[1]);
        throw;
    }
    if (pid == 0) {
        execChild(parentPid, program, args, pipeFds[1], stderrFd);
    }
    close(pipeFds[1]);
    return Spawned{.pid = pid, .stdoutFd = pipeFds[0]};
}

} // namespace nixcieval
