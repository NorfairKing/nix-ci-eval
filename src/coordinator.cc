#include "coordinator.hh"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <pthread.h>
#include <set>
#include <string>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <nix/util/signals.hh>

#include "line_io.hh"
#include "nix_eval.hh"
#include "pure.hh"
#include "worker.hh"

namespace nixcieval {

namespace {

using nlohmann::json;

// A worker slot that crashes this many times before ever becoming ready aborts
// the whole run, to bound a startup crash loop (e.g. the flake makes evaluation
// segfault or get OOM-killed).
constexpr int kMaxStartupFailures = 3;

// How deep an attribute path may get before the walk refuses to go further.
// Real outputs sit three levels down (packages.<system>.<name>) and a flake
// that groups them nests a few more, so this is far past anything meaningful
// while still bounding a set that contains itself.
constexpr std::size_t kMaxAttrPathDepth = 12;

// An attribute waiting to be evaluated, with how many workers have already
// died holding it. Carrying the count with the work is what lets a retry be
// distinguished from a first attempt after the worker that had it is gone.
struct PendingAttr {
    std::vector<std::string> path;
    int attempts = 0;
};

// A forked evaluation worker process and the parent-side pipe endpoints. Owns
// the process and the write descriptor; shutdown() (also run by the destructor)
// closes the pipe and reaps the child, so an exception anywhere in the run
// cannot orphan a child or leak a descriptor.
struct Worker {
    pid_t pid = -1;
    int toChildFd = -1; // parent -> child (write)
    std::unique_ptr<LineReader> fromChild;
    bool idle = false;
    // Whether this incarnation has reported "ready" at least once. A worker
    // that dies before its first "ready" crashed during startup.
    bool readySinceFork = false;
    // Consecutive startup crashes for this slot, carried across re-forks.
    int startupFailures = 0;
    // The attribute currently dispatched to this worker, if any. Used to
    // report an error if the worker dies mid-job.
    std::optional<PendingAttr> currentJob;
    // When this incarnation was forked, and when its current attribute was
    // dispatched, so the run can measure what workers and attributes cost.
    std::chrono::steady_clock::time_point forkedAt;
    std::chrono::steady_clock::time_point jobStartedAt;
    // Whether this incarnation has evaluated a derivation. Reaching its first
    // one also pays the worker's startup cost, which is what sizes the pool.
    bool producedADerivation = false;

    Worker() = default;
    ~Worker() { shutdown(); }

    Worker(const Worker &) = delete;
    Worker & operator=(const Worker &) = delete;
    Worker(Worker && other) noexcept { *this = std::move(other); }
    Worker & operator=(Worker && other) noexcept {
        if (this != &other) {
            shutdown();
            pid = other.pid;
            toChildFd = other.toChildFd;
            fromChild = std::move(other.fromChild);
            idle = other.idle;
            readySinceFork = other.readySinceFork;
            startupFailures = other.startupFailures;
            currentJob = std::move(other.currentJob);
            forkedAt = other.forkedAt;
            jobStartedAt = other.jobStartedAt;
            producedADerivation = other.producedADerivation;
            other.pid = -1;
            other.toChildFd = -1;
        }
        return *this;
    }

    // Close the pipe to the worker and wait for it to exit. Idempotent.
    void shutdown() {
        if (toChildFd >= 0) {
            close(toChildFd);
            toChildFd = -1;
        }
        fromChild.reset();
        if (pid > 0) {
            int status = 0;
            // Retrying is what keeps the child from being left a zombie for
            // the rest of the run when a signal lands mid-wait.
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            pid = -1;
        }
    }
};

// Fork one worker process. The child runs the evaluation loop and never
// returns; the parent keeps the pipe endpoints. 'inheritedFds' are the
// parent-side pipe descriptors of the other workers, which the child must
// close so it does not keep their pipes open.
Worker forkWorker(const Args & args, const std::vector<int> & inheritedFds) {
    int toChild[2];
    int fromChild[2];
    if (pipe(toChild) != 0) {
        throw std::runtime_error(std::string("pipe failed: ") +
                                 std::strerror(errno));
    }
    if (pipe(fromChild) != 0) {
        close(toChild[0]);
        close(toChild[1]);
        throw std::runtime_error(std::string("pipe failed: ") +
                                 std::strerror(errno));
    }

    pid_t parentPid = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        close(toChild[0]);
        close(toChild[1]);
        close(fromChild[0]);
        close(fromChild[1]);
        throw std::runtime_error(std::string("fork failed: ") +
                                 std::strerror(errno));
    }
    if (pid == 0) {
        // Child.
        //
        // Die with the coordinator. Without this a worker whose parent was
        // killed keeps evaluating, holding a core and its whole heap, long
        // after the caller believes the run is over. The getppid check closes
        // the window where the parent died before prctl was reached.
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parentPid) {
            _Exit(1);
        }
        // Nix blocks SIGINT, SIGTERM and friends and services them on a thread
        // of its own. A forked child inherits the mask but not the thread, so
        // leaving it in place would give the worker signals it has blocked and
        // nothing to act on them, and nothing short of SIGKILL could stop it.
        sigset_t empty;
        sigemptyset(&empty);
        pthread_sigmask(SIG_SETMASK, &empty, nullptr);
        // Keep a closed pipe a write error rather than a death, as it is in
        // the coordinator.
        signal(SIGPIPE, SIG_IGN);
        close(toChild[1]);
        close(fromChild[0]);
        for (int fd : inheritedFds) {
            close(fd);
        }
        runWorker(args, fromChild[1], toChild[0]);
        // runWorker never returns.
    }

    // Parent.
    close(toChild[0]);
    close(fromChild[1]);
    Worker worker;
    worker.pid = pid;
    worker.toChildFd = toChild[1];
    worker.fromChild = std::make_unique<LineReader>(fromChild[0]);
    worker.forkedAt = std::chrono::steady_clock::now();
    return worker;
}

// Emit one public NDJSON line, tagged with what kind of line it is so that a
// consumer dispatches on the tag rather than guessing from which fields happen
// to be present. Replaces the internal protocol tag, which is not the same
// vocabulary. Consumes its argument to avoid copying the (potentially large)
// record.
void emit(const char * type, json && line) {
    line.erase("t");
    line["type"] = type;
    std::string rendered = dumpLossy(line);
    rendered.push_back('\n');
    // A consumer that has gone away, or a full disk, must not look like a
    // flake with nothing in it. Unwritten output is the one failure this tool
    // cannot report through its output, so it says so on stderr and stops.
    // Workers die with the coordinator, so exiting here strands nothing.
    if (fwrite(rendered.data(), 1, rendered.size(), stdout) !=
            rendered.size() ||
        fflush(stdout) != 0) {
        fprintf(stderr, "nix-ci-eval: cannot write results: %s\n",
                std::strerror(errno));
        std::_Exit(1);
    }
}

// The ceiling on the worker pool. How many of these are actually forked is
// decided during the run by shouldSpawnWorker.
std::size_t maxWorkerCount(const Args & args) {
    if (args.workers) {
        return *args.workers;
    }
    unsigned hardware = std::thread::hardware_concurrency();
    return hardware == 0 ? 1 : hardware;
}

// Build the derivation reference graph reachable from 'roots': each derivation
// path mapped to its direct reference derivation paths. The store is queried in
// parallel by 'threads' workers, each with its own connection, so every
// reachable derivation is read exactly once regardless of how many outputs
// share it (the outputs' closures overlap heavily, so this is where the win is
// over walking each output's closure independently).
std::map<std::string, std::vector<std::string>>
buildReferenceGraph(const std::vector<std::string> & roots,
                    std::size_t threads) {
    std::map<std::string, std::vector<std::string>> graph;
    std::set<std::string> seen; // enqueued or being processed
    std::vector<std::string> queue;
    std::mutex mutex;
    std::condition_variable cv;
    std::size_t inFlight = 0;

    for (const auto & root : roots) {
        if (seen.insert(root).second) {
            queue.push_back(root);
        }
    }
    if (queue.empty()) {
        return graph;
    }

    auto worker = [&]() {
        std::shared_ptr<nix::Store> store;
        try {
            store = nix::openStore();
        } catch (const std::exception & e) {
            // Without a store this thread contributes no edges; the others
            // (or a degraded result) carry on rather than crashing the run.
            fprintf(stderr,
                    "nix-ci-eval: dependency worker could not open "
                    "store: %s\n",
                    e.what());
            return;
        }

        std::unique_lock<std::mutex> lock(mutex);
        while (true) {
            cv.wait(lock, [&] { return !queue.empty() || inFlight == 0; });
            if (queue.empty()) {
                // No work left and nothing in flight can produce more. Wake the
                // other threads so they can observe the same and finish.
                cv.notify_all();
                return;
            }

            std::string path = std::move(queue.back());
            queue.pop_back();
            inFlight++;
            lock.unlock();

            std::vector<std::string> references;
            try {
                auto info = store->queryPathInfo(store->parseStorePath(path));
                references.reserve(info->references.size());
                for (const auto & reference : info->references) {
                    references.push_back(store->printStorePath(reference));
                }
            } catch (const std::exception &) {
                // A path we cannot query (e.g. not in the store) is a leaf.
            }

            lock.lock();
            std::size_t added = 0;
            for (const auto & reference : references) {
                if (seen.insert(reference).second) {
                    queue.push_back(reference);
                    added++;
                }
            }
            graph.emplace(std::move(path), std::move(references));
            inFlight--;
            // Only wake others when there is new work to claim or the traversal
            // has finished; otherwise there is nothing for a waiter to do.
            if (added > 0 || inFlight == 0) {
                cv.notify_all();
            }
        }
    };

    // No point in more threads (or store connections) than there are roots to
    // start from.
    std::size_t poolSize = std::min(threads, roots.size());
    std::vector<std::thread> pool;
    pool.reserve(poolSize);
    for (std::size_t i = 0; i < poolSize; i++) {
        pool.emplace_back(worker);
    }
    for (auto & thread : pool) {
        thread.join();
    }
    return graph;
}

// Compute the dependency edges between discovered outputs and emit them.
void emitDependencies(
    const std::vector<std::pair<AttrPath, std::string>> & discovered,
    std::size_t threads) {
    // Map each derivation path to its attribute (first wins, so two outputs
    // that are the same derivation collapse to one name), and collect the
    // graph roots.
    std::map<std::string, AttrPath> drvPathToAttr;
    std::vector<std::string> roots;
    for (const auto & [attr, drvPath] : discovered) {
        if (drvPathToAttr.emplace(drvPath, attr).second) {
            roots.push_back(drvPath);
        }
    }

    // The graph is built in one shared traversal because the outputs' closures
    // overlap heavily. Only the edges are then computed one output at a time,
    // and emitted as they are, so a consumer reads them as a stream rather
    // than waiting for the last output to be walked.
    auto references = buildReferenceGraph(roots, threads);

    // Emit in discovery order for stable, predictable output.
    for (const auto & [attr, drvPath] : discovered) {
        auto dependencies =
            outputDependenciesFor(attr, drvPath, references, drvPathToAttr);
        if (dependencies.empty()) {
            continue;
        }
        // Keyed by attrPath, the same shape a job line uses, so a consumer
        // matches edges to jobs without parsing a rendered attribute back
        // apart.
        json line;
        line["attrPath"] = attr;
        line["dependencies"] = std::move(dependencies);
        emit("dependency", std::move(line));
    }
}

} // namespace

int runCoordinator(const Args & args) {
    initNixRuntime(args);

    std::size_t maxWorkers = maxWorkerCount(args);
    // A deque rather than a vector: the poll loop and the crash handler both
    // hold Worker references across a fork, and growing a deque leaves those
    // references valid.
    std::deque<Worker> workers;

    // The parent-side pipe descriptors of every live worker, so a newly forked
    // child can close the ones that are not its own.
    auto liveFdsExcept = [&](const Worker * skip) {
        std::vector<int> fds;
        for (const auto & worker : workers) {
            if (&worker == skip) {
                continue;
            }
            if (worker.toChildFd >= 0) {
                fds.push_back(worker.toChildFd);
            }
            if (worker.fromChild) {
                fds.push_back(worker.fromChild->fd());
            }
        }
        return fds;
    };

    std::deque<PendingAttr> todo;
    todo.push_back(PendingAttr{}); // the discovery root
    std::size_t active = 0;

    WorkerCostModel costModel;
    std::size_t evaluatedDerivations = 0;
    auto runStartedAt = std::chrono::steady_clock::now();

    // The pool state as the sizing policy sees it right now.
    auto poolSizing = [&]() {
        PoolSizing sizing;
        sizing.queued = todo.size();
        sizing.poolSize = workers.size();
        sizing.idleWorkers = static_cast<std::size_t>(
            std::count_if(workers.begin(), workers.end(),
                          [](const Worker & worker) { return worker.idle; }));
        sizing.evaluatedDerivations = evaluatedDerivations;
        sizing.maxWorkers = maxWorkers;
        sizing.elapsed = std::chrono::steady_clock::now() - runStartedAt;
        sizing.rampInterval = costModel.rampInterval();
        return sizing;
    };

    auto growPool = [&]() {
        if (!shouldSpawnWorker(poolSizing())) {
            return;
        }
        workers.push_back(forkWorker(args, liveFdsExcept(nullptr)));
    };

    // (attr, drvPath) of every kept output, for optional dependency discovery.
    std::vector<std::pair<AttrPath, std::string>> discovered;
    bool fatal = false;
    std::string fatalMessage;

    auto dispatch = [&]() {
        for (auto & worker : workers) {
            if (todo.empty()) {
                break;
            }
            if (!worker.idle) {
                continue;
            }
            auto pending = std::move(todo.front());
            todo.pop_front();
            json request;
            request["do"] = pending.path;
            worker.idle = false;
            worker.currentJob = std::move(pending);
            worker.jobStartedAt = std::chrono::steady_clock::now();
            active++;
            writeLine(worker.toChildFd, dumpLossy(request));
        }
    };

    auto handleLine = [&](Worker & worker, const std::string & raw) {
        json message = json::parse(raw);
        std::string kind = message.at("t");

        if (kind == "ready") {
            worker.idle = true;
            worker.readySinceFork = true;
            return;
        }
        if (kind == "restart") {
            worker.shutdown();
            worker = forkWorker(args, liveFdsExcept(&worker));
            return;
        }
        if (kind == "fatal") {
            fatal = true;
            fatalMessage = message.value("error", "worker failed");
            return;
        }

        // A job result: the dispatched attribute is done. Time it only if it
        // answers something actually dispatched, so a stray result cannot pass
        // an unset start time off as an enormous evaluation and stall the pool.
        auto now = std::chrono::steady_clock::now();
        if (kind == "job") {
            evaluatedDerivations++;
        }
        if (kind == "job" && worker.currentJob) {
            if (worker.producedADerivation) {
                costModel.recordAttr(now - worker.jobStartedAt);
            } else {
                // A worker's first derivation also paid for loading the flake
                // and forcing the shared evaluation substrate, which together
                // are what another worker would cost. Timing only later ones
                // as ordinary work is what keeps that cost visible: a pool
                // that starts up all at once has no warm worker to compare
                // against, and averaging the two together would hide it.
                costModel.recordFirstResult(now - worker.forkedAt);
                worker.producedADerivation = true;
            }
        }
        worker.currentJob.reset();
        if (active > 0) {
            active--;
        }

        if (kind == "children") {
            auto base = message.at("attrPath").get<std::vector<std::string>>();
            if (base.size() >= kMaxAttrPathDepth) {
                // An attribute set that contains itself, directly or through a
                // flake that re-exports its own outputs, would otherwise be
                // walked forever: every level looks like new work, so the
                // queue never empties and the same derivation is reported
                // under endlessly lengthening paths. Depth is the one bound
                // that holds whatever shape the cycle takes.
                nlohmann::json tooDeep;
                tooDeep["attrPath"] = base;
                tooDeep["error"] =
                    "attribute path is nested more than " +
                    std::to_string(kMaxAttrPathDepth) +
                    " levels deep; refusing to recurse further, which usually "
                    "means an attribute set contains itself";
                emit("error", std::move(tooDeep));
                return;
            }
            for (const auto & child :
                 message.at("children").get<std::vector<std::string>>()) {
                auto childPath = base;
                childPath.push_back(child);
                todo.push_back(PendingAttr{std::move(childPath), 0});
            }
            return;
        }
        if (kind == "ignore") {
            return;
        }
        if (kind == "error") {
            emit("error", std::move(message));
            return;
        }
        if (kind == "job") {
            auto attrPath =
                message.at("attrPath").get<std::vector<std::string>>();
            if (args.dependencies) {
                discovered.emplace_back(
                    attrPath, message.at("drvPath").get<std::string>());
            }
            emit("job", std::move(message));
            return;
        }
    };

    // Handle a worker that died or produced unusable output: report its
    // in-flight attribute as an error, then either re-fork it or, if it keeps
    // crashing before ever becoming ready, abort the whole run.
    auto handleWorkerDeath = [&](Worker & worker, const std::string & reason) {
        if (worker.currentJob) {
            if (shouldRetryAfterWorkerDeath(worker.currentJob->attempts)) {
                auto retry = std::move(*worker.currentJob);
                retry.attempts++;
                // To the front: whatever killed the worker is more likely to
                // be reproduced while the run still looks the way it did, and
                // finding out sooner keeps a doomed attribute from being
                // retried at the very end of an otherwise finished run.
                todo.push_front(std::move(retry));
            } else {
                json err;
                err["attrPath"] = worker.currentJob->path;
                err["error"] = "evaluation worker " + reason +
                               " while evaluating this attribute, twice";
                emit("error", std::move(err));
            }
            worker.currentJob.reset();
            if (active > 0) {
                active--;
            }
        }

        int failures = worker.readySinceFork ? 0 : worker.startupFailures + 1;
        if (failures >= kMaxStartupFailures) {
            fatal = true;
            fatalMessage =
                "an evaluation worker repeatedly crashed during startup";
            return;
        }

        if (worker.pid > 0) {
            // Ensure the worker is really gone so shutdown() cannot block.
            kill(worker.pid, SIGKILL);
        }
        worker.shutdown();
        worker = forkWorker(args, liveFdsExcept(&worker));
        worker.startupFailures = failures;
    };

    // How long to wait for worker output before reconsidering the pool size.
    //
    // Never waits indefinitely: a termination signal is consumed by Nix's
    // signal-handler thread rather than interrupting poll(), so the loop has to
    // come back around on its own to notice it. This bound is what makes the
    // tool answer a SIGTERM while its workers are busy. It also keeps the ramp
    // wait, which is measured and could in principle be enormous, from
    // overflowing the int poll() takes.
    const int maxPollMs = 1000;
    auto pollTimeoutMs = [&]() -> int {
        auto sizing = poolSizing();
        if (!spawnPendingOnRamp(sizing)) {
            return maxPollMs;
        }
        auto remaining = timeUntilSpawnAllowed(sizing);
        auto millis =
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
        // Round a sub-millisecond wait up rather than down: truncating it to
        // zero would spin on poll() until the clock caught up.
        if (remaining > std::chrono::steady_clock::duration::zero() &&
            millis == std::chrono::milliseconds::zero()) {
            return 1;
        }
        if (millis.count() >= maxPollMs) {
            return maxPollMs;
        }
        return static_cast<int>(millis.count());
    };

    while (true) {
        // Nix's signal-handler thread turns SIGINT and SIGTERM into this flag
        // rather than letting them reach us directly, so asking is the only
        // way to find out. Without it the tool runs to completion no matter
        // what its caller sends, which for anything running under a timeout
        // means being escalated to SIGKILL and leaving workers behind.
        if (nix::isInterrupted()) {
            fatal = true;
            fatalMessage = "interrupted";
            break;
        }
        growPool();
        dispatch();
        if (todo.empty() && active == 0) {
            break;
        }

        std::vector<pollfd> pollfds;
        std::vector<Worker *> pollWorkers;
        for (auto & worker : workers) {
            if (worker.fromChild) {
                pollfds.push_back({worker.fromChild->fd(), POLLIN, 0});
                pollWorkers.push_back(&worker);
            }
        }

        int ready = poll(pollfds.data(), pollfds.size(), pollTimeoutMs());
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("poll failed: ") +
                                     std::strerror(errno));
        }
        if (ready == 0) {
            // Timed out waiting for the ramp; go around to grow the pool.
            continue;
        }

        for (std::size_t i = 0; i < pollfds.size(); i++) {
            if ((pollfds[i].revents &
                 (POLLIN | POLLHUP | POLLERR | POLLNVAL)) == 0) {
                continue;
            }
            Worker * worker = pollWorkers[i];
            do {
                std::string line;
                try {
                    line = worker->fromChild->readLine();
                } catch (const LineTooLong &) {
                    handleWorkerDeath(*worker, "produced an oversized message");
                    break;
                }
                if (line.empty()) {
                    handleWorkerDeath(*worker, "exited unexpectedly");
                    break;
                }
                try {
                    handleLine(*worker, line);
                } catch (const std::exception &) {
                    // A malformed or partial line means the worker crashed
                    // mid-write; treat it like an unexpected exit.
                    handleWorkerDeath(*worker, "produced malformed output");
                    break;
                }
                if (fatal) {
                    break;
                }
            } while (worker->fromChild && worker->fromChild->hasBufferedLine());
        }

        if (fatal) {
            break;
        }
    }

    for (auto & worker : workers) {
        if (worker.toChildFd >= 0) {
            writeLine(worker.toChildFd, R"({"exit":true})");
        }
    }
    // A worker may still be mid-evaluation and will not read the exit command
    // until it finishes, so shutdown()'s blocking waitpid would wait out a
    // whole evaluation. That is not only the fatal path: the pool can have
    // grown a worker that is still loading the flake while the others drained
    // the queue, and waiting for it would hold every result back for no
    // reason. Workers keep nothing that needs flushing, so kill them all.
    for (auto & worker : workers) {
        if (worker.pid > 0) {
            kill(worker.pid, SIGKILL);
        }
    }
    for (auto & worker : workers) {
        worker.shutdown();
    }

    if (fatal) {
        fprintf(stderr, "nix-ci-eval: %s\n", fatalMessage.c_str());
        return 1;
    }

    if (args.dependencies) {
        emitDependencies(discovered, maxWorkers);
    }

    // Say that the stream is complete, as its last act.
    //
    // Nothing else can say it. An output with no dependencies emits no edge,
    // and --no-dependencies emits none at all, so a consumer cannot tell a
    // finished run from one whose output was cut off partway: both simply
    // stop. Reaching this line is the only proof that everything the run was
    // going to report has been reported.
    emit("done", json::object());

    return 0;
}

} // namespace nixcieval
