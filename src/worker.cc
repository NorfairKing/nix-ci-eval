#include "worker.hh"

#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <cstdio>
#include <unistd.h>

#include "line_io.hh"
#include "nix_eval.hh"
#include "pure.hh"

namespace nixcieval {

std::string dumpLossy(const nlohmann::json & value) {
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

namespace {

using nlohmann::json;

// A ceiling on how much of a Nix error message travels on one line. Error
// text is whatever the flake author wrote, and a flake can produce an
// arbitrarily large one; the reason survives, the flood does not.
constexpr std::size_t kMaxMessageBytes = 64 * 1024;

// How much this worker is holding right now, in MiB.
//
// Read from /proc rather than getrusage, whose ru_maxrss is a lifetime peak
// that never falls: with the peak, one expensive attribute would leave a
// worker over the ceiling forever and force a restart after every subsequent
// attribute, re-forcing the whole evaluation substrate each time.
std::size_t currentResidentMiB() {
    std::FILE * statm = std::fopen("/proc/self/statm", "r");
    if (statm == nullptr) {
        return 0;
    }
    char buffer[256] = {};
    std::size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, statm);
    std::fclose(statm);
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) {
        return 0;
    }
    return residentMiBFromStatm(std::string(buffer, read),
                                static_cast<std::size_t>(pageSize));
}

[[noreturn]] void fatalToParent(int toParentFd, const std::string & message) {
    json err;
    err["t"] = "fatal";
    err["error"] = message;
    writeLine(toParentFd, dumpLossy(err));
    std::_Exit(1);
}

json jobToJson(const std::vector<std::string> & attrPath,
               const EvalOutcome & outcome) {
    json result;
    result["t"] = "job";
    result["attrPath"] = attrPath;
    result["drvPath"] = outcome.drvPath;
    result["isCached"] = outcome.isCached;
    return result;
}

// The worker's request loop. Extracted so runWorker can wrap it: a forked
// child must never let an exception unwind into the parent's stack frames,
// where it would run parent-only teardown (waitpid/close) on shared state.
// Never returns (it only ever std::_Exit()s), which is what lets runWorker
// stay [[noreturn]].
[[noreturn]] void runWorkerLoop(const Args & args, int toParentFd,
                                int fromParentFd) {
    LineReader fromParent(fromParentFd);

    std::unique_ptr<Evaluator> evaluator;
    try {
        evaluator = std::make_unique<Evaluator>(args);
    } catch (const std::exception & e) {
        fatalToParent(
            toParentFd,
            "failed to evaluate flake: " +
                truncateForLine(cleanErrorMessage(e.what()), kMaxMessageBytes));
    }

    while (true) {
        if (!writeLine(toParentFd, R"({"t":"ready"})")) {
            std::_Exit(0);
        }

        std::string command = fromParent.readLine();
        if (command.empty()) {
            std::_Exit(0); // parent closed the pipe
        }

        json request;
        try {
            request = json::parse(command);
        } catch (const std::exception & e) {
            fatalToParent(toParentFd,
                          std::string("could not parse request: ") + e.what());
        }

        if (request.contains("exit")) {
            std::_Exit(0);
        }
        if (!request.contains("do")) {
            fatalToParent(toParentFd, "request had neither 'do' nor 'exit'");
        }

        auto attrPath = request.at("do").get<std::vector<std::string>>();

        json response;
        try {
            EvalOutcome outcome = evaluator->evalAttr(attrPath);
            switch (outcome.kind) {
            case OutcomeKind::Job:
                response = jobToJson(attrPath, outcome);
                break;
            case OutcomeKind::Children:
                response["t"] = "children";
                response["attrPath"] = attrPath;
                response["children"] = outcome.children;
                break;
            case OutcomeKind::Ignore:
                response["t"] = "ignore";
                response["attrPath"] = attrPath;
                break;
            }
        } catch (const std::exception & e) {
            response["t"] = "error";
            response["attrPath"] = attrPath;
            response["error"] =
                truncateForLine(cleanErrorMessage(e.what()), kMaxMessageBytes);
        }

        if (!writeLine(toParentFd, dumpLossy(response))) {
            std::_Exit(0);
        }

        // Recycle once this worker is over the ceiling, so a long-running
        // evaluator cannot grow without bound.
        if (currentResidentMiB() > args.maxMemoryMiB) {
            writeLine(toParentFd, R"({"t":"restart"})");
            std::_Exit(0);
        }
    }
}

} // namespace

void runWorker(const Args & args, int toParentFd, int fromParentFd) {
    // A forked child must never let an exception unwind into the parent's
    // stack frames. Guard the whole loop so any escape (e.g. a malformed
    // request, or bad_alloc) becomes a fatal message to the parent and a clean
    // exit, rather than running parent teardown on shared state.
    try {
        runWorkerLoop(args, toParentFd, fromParentFd);
    } catch (const std::exception & e) {
        fatalToParent(
            toParentFd,
            std::string("worker error: ") +
                truncateForLine(cleanErrorMessage(e.what()), kMaxMessageBytes));
    } catch (...) {
        fatalToParent(toParentFd, "worker error: unknown exception");
    }
}

} // namespace nixcieval
