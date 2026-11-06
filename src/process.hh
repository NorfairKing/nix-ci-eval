#pragma once
// Running other programs (git, iets) from the coordinator.

#include <string>
#include <sys/types.h>
#include <vector>

namespace nixcieval {

// Run a program to completion with stdin closed and stderr shared, and return
// whether it exited successfully. When 'capturedStdout' is given, the
// program's stdout is collected into it; otherwise it goes to stderr, so that
// a chatty program cannot write into this tool's output stream.
//
// 'program' is looked up on PATH when it contains no slash.
bool runProgram(const std::string & program,
                const std::vector<std::string> & args,
                std::string * capturedStdout = nullptr);

// A program started with its stdout on a pipe.
struct Spawned {
    pid_t pid = -1;
    // The read end of the program's stdout, owned by the caller.
    int stdoutFd = -1;
};

// Start a program with stdin closed, stdout on a pipe and stderr on
// 'stderrFd'. The program dies with this process.
Spawned spawnProgram(const std::string & program,
                     const std::vector<std::string> & args, int stderrFd);

// Wait for a program to exit and say whether it exited successfully.
bool waitSucceeded(pid_t pid);

} // namespace nixcieval
