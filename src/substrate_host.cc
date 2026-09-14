#include "substrate_host.hh"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <pthread.h>
#include <string>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

#include "fd_passing.hh"
#include "nix_eval.hh"
#include "pure.hh"
#include "trace.hh"
#include "worker.hh"

namespace nixcieval {

namespace {

using nlohmann::json;

// A ceiling on how much of a Nix error message travels on one message, as in
// the worker protocol: a flake decides how long its error text is.
constexpr std::size_t kMaxMessageBytes = 64 * 1024;

// A worker's two pipe endpoints, and nothing else, arrive with a fork request.
constexpr std::size_t kFdsPerRequest = 2;

bool tell(int controlFd, const json & message) {
    return sendWithFds(controlFd, dumpLossy(message), {});
}

[[noreturn]] void fatalToCoordinator(int controlFd,
                                     const std::string & message) {
    json fatal;
    fatal["t"] = "fatal";
    fatal["error"] = message;
    tell(controlFd, fatal);
    std::_Exit(1);
}

// Whether this process is the only thread in it.
//
// Forking a worker is only safe while it is. Nix starts background threads of
// its own (the file transfer's curl thread is the one to expect), and a thread
// does not survive into the child: a worker that inherited a library whose
// worker thread is gone would block forever the first time it used it. The
// host is forked from the coordinator before any evaluation, so the answer is
// yes unless loading the flake started something.
bool isSingleThreaded() {
    std::FILE * status = std::fopen("/proc/self/status", "r");
    if (status == nullptr) {
        // Without an answer, assume the unsafe one: the coordinator then forks
        // its own workers, which is slower and always correct.
        return false;
    }
    char line[256];
    long threads = -1;
    while (std::fgets(line, sizeof(line), status) != nullptr) {
        if (std::sscanf(line, "Threads: %ld", &threads) == 1) {
            break;
        }
    }
    std::fclose(status);
    return threads == 1;
}

// Fetch whatever the flake needs, in a child process that then exits.
//
// Downloading starts nix's file-transfer thread, and that thread lives as long
// as the process it was started in. A thread does not survive a fork, so a
// worker forked from a host that had downloaded anything would inherit a file
// transfer with nothing behind it and hang on the first thing it asked for.
// The host refuses to fork workers at all in that case, which costs the run
// the whole point of having a host: a cold store is exactly when a worker is
// most expensive.
//
// Paying for the fetch over here leaves the store and the fetcher cache warm
// and this process untouched, so that loading the flake below downloads
// nothing.
void prefetchFlake(const Args & args) {
    pid_t pid = fork();
    if (pid < 0) {
        // Nothing gained; the host loads the flake itself either way.
        return;
    }
    if (pid == 0) {
        try {
            fetchFlake(args);
        } catch (const std::exception &) {
            // A flake that cannot be fetched is reported by the host, which is
            // about to try the same thing and fail the same way.
        }
        std::_Exit(0);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
}

// Fork one worker that serves requests on the endpoints the coordinator sent.
// Returns the worker's pid in the host; the worker itself never returns.
pid_t forkInheritedWorker(const Args & args, Evaluator & evaluator,
                          int controlFd, int fromCoordinatorFd,
                          int toCoordinatorFd) {
    pid_t hostPid = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid > 0) {
        return pid;
    }

    // Worker.
    //
    // Die with the host, which dies with the coordinator, so a worker cannot
    // outlive the run that wanted it however the run ends. The getppid check
    // closes the window where the host died before prctl was reached.
    prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (getppid() != hostPid) {
        _Exit(1);
    }
    sigset_t empty;
    sigemptyset(&empty);
    pthread_sigmask(SIG_SETMASK, &empty, nullptr);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_DFL);
    close(controlFd);
    runInheritedWorker(args, evaluator, toCoordinatorFd, fromCoordinatorFd);
    // runInheritedWorker never returns.
}

[[noreturn]] void runSubstrateHostLoop(const Args & args, int controlFd) {
    auto hostStarted = std::chrono::steady_clock::now();
    prefetchFlake(args);
    if (traceEnabled()) {
        fprintf(stderr, "trace host fetched the flake in %.3fs\n",
                traceSecondsSince(hostStarted));
    }
    std::unique_ptr<Evaluator> evaluator;
    try {
        auto loadStarted = std::chrono::steady_clock::now();
        evaluator = std::make_unique<Evaluator>(args);
        if (traceEnabled()) {
            fprintf(stderr, "trace host loaded the flake in %.3fs\n",
                    traceSecondsSince(loadStarted));
        }
    } catch (const std::exception & e) {
        fatalToCoordinator(
            controlFd,
            "failed to evaluate flake: " +
                truncateForLine(cleanErrorMessage(e.what()), kMaxMessageBytes));
    }

    if (!isSingleThreaded()) {
        json unsafe;
        unsafe["t"] = "cannotFork";
        tell(controlFd, unsafe);
        std::_Exit(0);
    }

    // Force something, anything, before the first worker is forked: whatever
    // the run turns out to work on, it shares most of its derivation graph
    // with the first derivation in the flake.
    {
        auto started = std::chrono::steady_clock::now();
        evaluator->forceSubstrate();
        if (traceEnabled()) {
            fprintf(stderr, "trace host forced the substrate in %.3fs\n",
                    traceSecondsSince(started));
        }
    }

    // Nothing here waits for a worker, so let the kernel reap them rather than
    // leaving one zombie per worker the run recycles.
    signal(SIGCHLD, SIG_IGN);

    json ready;
    ready["t"] = "ready";
    if (!tell(controlFd, ready)) {
        std::_Exit(0);
    }

    while (true) {
        FdMessage request = receiveWithFds(controlFd, kFdsPerRequest);
        if (request.closed) {
            std::_Exit(0); // the coordinator is gone
        }

        json parsed;
        try {
            parsed = json::parse(request.message);
        } catch (const std::exception &) {
            for (int fd : request.fds) {
                close(fd);
            }
            fatalToCoordinator(controlFd, "could not parse fork request");
        }

        if (parsed.contains("exit")) {
            std::_Exit(0);
        }

        // Forcing everything on the way to an attribute the run has already
        // seen through is what makes the next worker warm: the coordinator
        // names an attribute that turned out to be a derivation, and this
        // leaves what it shares forced in the heap every later worker
        // inherits. The coordinator forks nothing while it waits for this.
        if (parsed.contains("prime")) {
            auto attrPath = parsed.at("prime").get<std::vector<std::string>>();
            auto started = std::chrono::steady_clock::now();
            evaluator->forceSubstrateFor(attrPath);
            if (traceEnabled()) {
                fprintf(stderr, "trace host forced what %s shares in %.3fs\n",
                        attrPath.empty() ? "?" : attrPath.front().c_str(),
                        traceSecondsSince(started));
            }
            json primed;
            primed["t"] = "primed";
            if (!tell(controlFd, primed)) {
                std::_Exit(0);
            }
            continue;
        }

        if (request.fds.size() != kFdsPerRequest) {
            for (int fd : request.fds) {
                close(fd);
            }
            fatalToCoordinator(controlFd,
                               "fork request did not carry a worker's pipes");
        }

        int fromCoordinatorFd = request.fds[0];
        int toCoordinatorFd = request.fds[1];
        pid_t pid = forkInheritedWorker(args, *evaluator, controlFd,
                                        fromCoordinatorFd, toCoordinatorFd);
        // The worker holds these now; the host keeping them open would keep
        // the coordinator from ever seeing that worker's pipe close.
        close(fromCoordinatorFd);
        close(toCoordinatorFd);

        json answer;
        if (pid < 0) {
            answer["t"] = "forkFailed";
        } else {
            answer["t"] = "forked";
            answer["pid"] = pid;
        }
        if (!tell(controlFd, answer)) {
            std::_Exit(0);
        }
    }
}

} // namespace

void runSubstrateHost(const Args & args, int controlFd) {
    // A forked child must never let an exception unwind into the frames of the
    // process it was forked from, where it would run that process's teardown
    // on shared state.
    try {
        runSubstrateHostLoop(args, controlFd);
    } catch (const std::exception & e) {
        fatalToCoordinator(
            controlFd,
            std::string("substrate host error: ") +
                truncateForLine(cleanErrorMessage(e.what()), kMaxMessageBytes));
    } catch (...) {
        fatalToCoordinator(controlFd,
                           "substrate host error: unknown exception");
    }
}

} // namespace nixcieval
