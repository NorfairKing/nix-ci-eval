#pragma once

#include <memory>
#include <string>
#include <sys/types.h>
#include <variant>

#include "args.hh"
#include "iets.hh"
#include "line_io.hh"

namespace nixcieval {

// A running `iets eval-jobs` over a checkout of the root flake, and the
// temporary directory holding that checkout and iets's log. Destroying it
// kills iets if it is still running and removes the directory.
class IetsRun {
  public:
    // Check out the root flake and start iets on it, or say why this run is
    // evaluated with Nix instead.
    static std::variant<std::unique_ptr<IetsRun>, NotForIets>
    start(const Args & args);

    ~IetsRun();
    IetsRun(const IetsRun &) = delete;
    IetsRun & operator=(const IetsRun &) = delete;

    LineReader & output() { return *output_; }

    // Reap iets after its output has ended, and say whether it exited
    // successfully. A failure means some attributes may never have been
    // reported.
    bool finish();

    // Stop iets early, because its output can no longer be trusted.
    void abandon();

    // The end of what iets wrote to stderr, for explaining a failure.
    std::string logTail() const;

  private:
    IetsRun(std::string tempDir, pid_t pid, int stdoutFd);

    std::string tempDir_;
    pid_t pid_ = -1;
    std::unique_ptr<LineReader> output_;
};

} // namespace nixcieval
