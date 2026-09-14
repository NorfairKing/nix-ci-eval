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
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <nix/util/signals.hh>

#include "fd_passing.hh"
#include "line_io.hh"
#include "nix_eval.hh"
#include "pure.hh"
#include "substrate_host.hh"
#include "trace.hh"
#include "worker.hh"

namespace nixcieval {

namespace {

using nlohmann::json;

// What a run cost, for NIX_CI_EVAL_TRACE (see trace.hh). A worker's first
// derivation is timed from its fork, because reaching it is what a worker
// costs to start; every later one is timed from its dispatch, because that is
// what an attribute costs once a worker is warm.
struct RunTrace {
    std::chrono::steady_clock::time_point start =
        std::chrono::steady_clock::now();
    std::size_t forks = 0;
    std::size_t firstResults = 0;
    std::chrono::steady_clock::duration firstResultTotal{};
    std::size_t warmAttrs = 0;
    std::chrono::steady_clock::duration warmAttrTotal{};
    std::size_t jobs = 0;
};
RunTrace trace;

// A worker slot that crashes this many times before ever becoming ready aborts
// the whole run, to bound a startup crash loop (e.g. the flake makes evaluation
// segfault or get OOM-killed).
constexpr int kMaxStartupFailures = 3;

// How deep an attribute path may get before the walk refuses to go further.
// Real outputs sit three levels down (packages.<system>.<name>) and a flake
// that groups them nests a few more, so this is far past anything meaningful
// while still bounding a set that contains itself.
constexpr std::size_t kMaxAttrPathDepth = 12;

// How many reference queries one pass of the loop answers. Small enough that
// servicing a worker is never held up for long, large enough that the walk
// keeps pace with discovery.
constexpr int kEdgeQueriesPerPass = 64;

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
    // Whether the substrate host forked this worker rather than the
    // coordinator. Such a worker is a grandchild: it can be signalled but not
    // waited for, and the host is what reaps it.
    bool forkedByHost = false;
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
            forkedByHost = other.forkedByHost;
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
            if (forkedByHost) {
                // A grandchild cannot be waited for here. It has been
                // signalled by now, and the host reaps what it forked.
                pid = -1;
                return;
            }
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
    trace.forks++;
    if (traceEnabled()) {
        fprintf(stderr, "trace %.3f forked worker %d itself (worker %zu)\n",
                traceSecondsSince(trace.start), (int)pid, trace.forks);
    }
    return worker;
}

// The process that holds the flake's forced evaluator and forks workers from
// it, with the coordinator's end of the socket it is spoken to on.
//
// Owns the process and the socket; shutdown() (also run by the destructor)
// closes the socket and reaps the host, so an exception anywhere in the run
// cannot orphan it.
struct SubstrateHost {
    pid_t pid = -1;
    int controlFd = -1;
    // Whether the host has finished loading the flake and forcing what its
    // outputs share, which is when workers can be forked from it.
    bool ready = false;
    // Cleared the moment the host cannot be used, after which the coordinator
    // forks its own workers: slower, and always correct.
    bool usable = false;
    // Whether the host is busy forcing what an attribute shares. It answers
    // nothing else while it is, so nothing is asked of it meanwhile.
    bool priming = false;

    SubstrateHost() = default;
    ~SubstrateHost() { shutdown(); }

    SubstrateHost(const SubstrateHost &) = delete;
    SubstrateHost & operator=(const SubstrateHost &) = delete;

    void shutdown() {
        usable = false;
        ready = false;
        if (controlFd >= 0) {
            close(controlFd);
            controlFd = -1;
        }
        if (pid > 0) {
            // It is only ever killed, never asked to stop: it holds nothing
            // that needs flushing, and a host part-way through forcing an
            // attribute would otherwise be waited out.
            kill(pid, SIGKILL);
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
            pid = -1;
        }
    }
};

// Fork the substrate host. It loads the flake and forces the substrate before
// it answers anything, so it is not usable until it has said that it is ready.
void forkSubstrateHost(const Args & args, SubstrateHost & host) {
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) != 0) {
        throw std::runtime_error(std::string("socketpair failed: ") +
                                 std::strerror(errno));
    }

    pid_t parentPid = getpid();
    pid_t pid = fork();
    if (pid < 0) {
        close(pair[0]);
        close(pair[1]);
        throw std::runtime_error(std::string("fork failed: ") +
                                 std::strerror(errno));
    }
    if (pid == 0) {
        // Host. Same treatment as a worker: die with the coordinator, and
        // service signals rather than inheriting a mask with no thread behind
        // it.
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parentPid) {
            _Exit(1);
        }
        sigset_t empty;
        sigemptyset(&empty);
        pthread_sigmask(SIG_SETMASK, &empty, nullptr);
        signal(SIGPIPE, SIG_IGN);
        close(pair[0]);
        runSubstrateHost(args, pair[1]);
        // runSubstrateHost never returns.
    }

    close(pair[1]);
    host.pid = pid;
    host.controlFd = pair[0];
    host.usable = true;
}

// Thrown when the substrate host could not fork a worker. The run answers it
// by forking its own.
struct HostCannotFork {};

// The most a request to the host may occupy. Its receiving end reads into a
// fixed buffer, and a message larger than that arrives truncated rather than
// split, so this is kept well under it.
constexpr std::size_t kMaxHostRequestBytes = 3072;

// How long to wait for the host to answer a fork request. It evaluates nothing
// once it is ready, so the answer is immediate; the bound is only so that a
// wedged host costs the run a fallback rather than the run.
constexpr int kHostReplyTimeoutMs = 10000;

// Have the substrate host fork a worker onto pipes created here, so that a
// worker forked over there is spoken to exactly like one forked here.
Worker forkWorkerViaHost(SubstrateHost & host) {
    int toChild[2];
    int fromChild[2];
    if (pipe(toChild) != 0) {
        throw HostCannotFork{};
    }
    if (pipe(fromChild) != 0) {
        close(toChild[0]);
        close(toChild[1]);
        throw HostCannotFork{};
    }

    auto giveUp = [&]() {
        close(toChild[1]);
        close(fromChild[0]);
        host.usable = false;
        throw HostCannotFork{};
    };

    bool sent = sendWithFds(host.controlFd, R"({"fork":true})",
                            {toChild[0], fromChild[1]});
    // The host holds the worker's ends now, whether or not it forks: keeping
    // them open here would keep the coordinator from ever seeing the worker's
    // pipe close.
    close(toChild[0]);
    close(fromChild[1]);
    if (!sent) {
        giveUp();
    }

    json answer;
    while (true) {
        pollfd waiting{host.controlFd, POLLIN, 0};
        int ready = poll(&waiting, 1, kHostReplyTimeoutMs);
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            giveUp();
        }
        try {
            FdMessage reply = receiveWithFds(host.controlFd, 0);
            if (reply.closed) {
                giveUp();
            }
            answer = json::parse(reply.message);
        } catch (const std::exception &) {
            giveUp();
        }
        // A priming this was not waiting for can land first, since the host
        // answers in the order it was asked.
        if (answer.value("t", "") == "primed") {
            host.priming = false;
            continue;
        }
        break;
    }
    pid_t forkedPid = -1;
    try {
        if (answer.value("t", "") != "forked") {
            giveUp();
        }
        forkedPid = answer.at("pid").get<pid_t>();
    } catch (const HostCannotFork &) {
        throw;
    } catch (const std::exception &) {
        // An answer without a usable pid is an answer this cannot act on.
        giveUp();
    }

    Worker worker;
    worker.pid = forkedPid;
    worker.forkedByHost = true;
    worker.toChildFd = toChild[1];
    worker.fromChild = std::make_unique<LineReader>(fromChild[0]);
    worker.forkedAt = std::chrono::steady_clock::now();
    trace.forks++;
    if (traceEnabled()) {
        fprintf(stderr, "trace %.3f host forked worker %d (worker %zu, warm)\n",
                traceSecondsSince(trace.start), (int)worker.pid, trace.forks);
    }
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

// Report the edges an EdgeDiscovery has just worked out, one line per
// depending attribute.
//
// An attribute can appear in several dependency lines over a run: an edge is
// reported as soon as both of its ends are known, rather than held back until
// everything that attribute depends on has been found. A consumer therefore
// merges the lines for an attribute rather than taking the last one as its
// answer.
void emitEdges(const std::vector<OutputEdge> & edges) {
    std::map<AttrPath, std::vector<AttrPath>> byFrom;
    for (const auto & edge : edges) {
        byFrom[edge.from].push_back(edge.to);
    }
    for (auto & [from, to] : byFrom) {
        json line;
        line["attrPath"] = from;
        line["dependencies"] = std::move(to);
        emit("dependency", std::move(line));
    }
}

} // namespace

int runCoordinator(const Args & args) {
    initNixRuntime(args);

    // The process every worker is forked from, so that the flake is loaded and
    // its shared substrate forced once for the run rather than once per
    // worker. Forked before anything else, so that it inherits no pipes and
    // starts loading while the coordinator is still setting up.
    SubstrateHost host;
    try {
        forkSubstrateHost(args, host);
    } catch (const std::exception & e) {
        // Without it every worker loads the flake itself, which is what this
        // tool did before there was a host: slower, and correct.
        fprintf(stderr,
                "nix-ci-eval: cannot share the evaluation substrate: %s\n",
                e.what());
    }

    std::size_t maxWorkers = maxWorkerCount(args);
    // A deque rather than a vector: the poll loop and the crash handler both
    // hold Worker references across a fork, and growing a deque leaves those
    // references valid.
    std::deque<Worker> workers;

    // The parent-side pipe descriptors of every live worker, so a newly forked
    // child can close the ones that are not its own.
    auto liveFdsExcept = [&](const Worker * skip) {
        std::vector<int> fds;
        if (host.controlFd >= 0) {
            fds.push_back(host.controlFd);
        }
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

    // The top-level output types the host has been primed for.
    std::set<std::string> primedOutputTypes;

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

    // Fork one worker, from the substrate host while there is one. 'replacing'
    // is the worker being replaced, if any, whose descriptors a worker the
    // coordinator forks itself must not inherit.
    auto spawnWorker = [&](const Worker * replacing) -> Worker {
        // Not while the host is forcing what an attribute shares: it answers
        // nothing until it is done, and that can take as long as evaluating an
        // attribute does. A worker forked here instead loads the flake
        // itself, which is what every worker used to do.
        if (host.usable && host.ready && !host.priming) {
            try {
                return forkWorkerViaHost(host);
            } catch (const HostCannotFork &) {
                // From here on the coordinator forks its own workers, each
                // loading the flake for itself.
            }
        }
        return forkWorker(args, liveFdsExcept(replacing));
    };

    auto growPool = [&]() {
        if (!shouldSpawnWorker(poolSizing())) {
            return;
        }
        // A host that is still loading the flake, or still forcing what an
        // attribute shares, is worth waiting for: a worker forked afterwards
        // inherits that work, and one forked now would pay for it again.
        if (host.usable && (!host.ready || host.priming)) {
            return;
        }
        workers.push_back(spawnWorker(nullptr));
    };

    // (attr, drvPath) of every kept output, for optional dependency discovery.
    // Edges are worked out as the run goes, in the coordinator time that is
    // otherwise spent waiting on workers, so each one is reported as soon as
    // both of its ends are known.
    EdgeDiscovery edgeDiscovery;
    // Cleared if the store turns out to be unreadable, so the run reports what
    // it can rather than stopping.
    bool wantEdges = args.dependencies;
    // Opened on first use and only when edges are wanted, so a run that does
    // not ask for them never opens a store at all.
    std::shared_ptr<nix::Store> edgeStore;
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

    // Ask the host to force what an attribute shares, so that every worker
    // forked afterwards inherits it rather than forcing it again.
    //
    // Once per top-level output type, and only for an attribute that turned
    // out to be a derivation: reaching one package forces the package set it
    // came from, but a flake's checks and its packages can be built from
    // different ones, and an attribute picked before the run has seen any
    // derivation can lead into a corner of the flake that shares nothing with
    // the work that follows.
    auto primeHostFor = [&](const std::vector<std::string> & attrPath) {
        if (!host.usable || !host.ready || host.priming || attrPath.empty()) {
            return;
        }
        if (!primedOutputTypes.insert(attrPath.front()).second) {
            return;
        }
        json request;
        request["prime"] = attrPath;
        std::string rendered = dumpLossy(request);
        // One send is one receive, and the receiving end reads into a fixed
        // buffer, so an attribute path long enough to overflow it would arrive
        // truncated. Such an attribute is not worth priming for.
        if (rendered.size() > kMaxHostRequestBytes) {
            return;
        }
        if (!sendWithFds(host.controlFd, rendered, {})) {
            host.usable = false;
            return;
        }
        host.priming = true;
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
            worker = spawnWorker(&worker);
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
            trace.jobs++;
        }
        if (kind == "job" && worker.currentJob) {
            if (worker.producedADerivation) {
                costModel.recordAttr(now - worker.jobStartedAt);
                trace.warmAttrs++;
                trace.warmAttrTotal += now - worker.jobStartedAt;
            } else {
                // A worker's first derivation also paid for whatever it had to
                // load and force rather than inherit, which is what another
                // worker would cost. Timing only later ones as ordinary work
                // is what keeps that cost visible: a pool that starts up all
                // at once has no warm worker to compare against, and
                // averaging the two together would hide it.
                costModel.recordFirstResult(now - worker.forkedAt);
                trace.firstResults++;
                trace.firstResultTotal += now - worker.forkedAt;
                if (traceEnabled()) {
                    fprintf(stderr,
                            "trace %.3f worker %d reached its first derivation "
                            "%.3fs after being forked (the attribute itself "
                            "took %.3fs)\n",
                            traceSecondsSince(trace.start), (int)worker.pid,
                            traceSeconds(now - worker.forkedAt),
                            traceSeconds(now - worker.jobStartedAt));
                }
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
            auto drvPath = message.at("drvPath").get<std::string>();
            primeHostFor(attrPath);
            // The job goes out before the edges its arrival makes known,
            // because those edges name it: a consumer told to order a build
            // against an attribute it has not been told about yet has nothing
            // to attach the edge to, and drops it.
            emit("job", std::move(message));
            if (wantEdges) {
                emitEdges(edgeDiscovery.addOutput(attrPath, drvPath));
            }
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
        worker = spawnWorker(&worker);
        worker.startupFailures = failures;
    };

    // Read one message from the substrate host. It says when it is ready to
    // fork workers, and otherwise only speaks up when something has gone
    // wrong; either way the run continues, with its own workers if it must.
    auto handleHostMessage = [&]() {
        // The message may already have been taken: forking a worker reads this
        // socket too, and can have done so between poll() and here. Reading
        // then would block the whole run on a message that has been handled.
        pollfd waiting{host.controlFd, POLLIN, 0};
        if (poll(&waiting, 1, 0) <= 0) {
            return;
        }
        FdMessage message;
        try {
            message = receiveWithFds(host.controlFd, 0);
        } catch (const std::exception &) {
            host.usable = false;
            return;
        }
        if (message.closed) {
            host.usable = false;
            return;
        }
        json parsed;
        try {
            parsed = json::parse(message.message);
        } catch (const std::exception &) {
            host.usable = false;
            return;
        }
        std::string kind = parsed.value("t", "");
        if (kind == "ready") {
            host.ready = true;
            return;
        }
        if (kind == "primed") {
            host.priming = false;
            return;
        }
        if (kind == "fatal") {
            // The host evaluates the flake before anything else does, so a
            // flake that cannot be evaluated at all is reported here, once,
            // rather than by every worker in turn.
            fatal = true;
            fatalMessage = parsed.value("error", "the substrate host failed");
            return;
        }
        // Anything else, "cannotFork" included, means the run forks its own
        // workers from here on.
        host.usable = false;
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

    // Answer a bounded number of reference queries, so the loop keeps
    // servicing workers rather than disappearing into a closure walk. The
    // coordinator is idle most of a run, waiting on evaluation, which is the
    // time this uses.
    auto stepEdges = [&](int budget) {
        if (!wantEdges) {
            return;
        }
        for (int i = 0; i < budget; i++) {
            auto query = edgeDiscovery.nextQuery();
            if (!query) {
                return;
            }
            if (!edgeStore) {
                try {
                    edgeStore = nix::openStore();
                } catch (const std::exception & e) {
                    // Without a store there are no edges to report. Say so
                    // once, rather than leaving a consumer to read their
                    // absence as "this flake has none".
                    fprintf(stderr,
                            "nix-ci-eval: cannot read dependencies: %s\n",
                            e.what());
                    wantEdges = false;
                    return;
                }
            }
            std::vector<std::string> references;
            try {
                auto info =
                    edgeStore->queryPathInfo(edgeStore->parseStorePath(*query));
                references.reserve(info->references.size());
                for (const auto & reference : info->references) {
                    references.push_back(edgeStore->printStorePath(reference));
                }
            } catch (const std::exception &) {
                // A path that cannot be queried is a leaf.
            }
            emitEdges(edgeDiscovery.provideReferences(*query, references));
        }
    };

    while (true) {
        // Nix.s signal-handler thread turns SIGINT and SIGTERM into this flag
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
        stepEdges(kEdgeQueriesPerPass);
        if (todo.empty() && active == 0) {
            break;
        }

        std::vector<pollfd> pollfds;
        // One entry per polled descriptor, naming the worker it belongs to, or
        // null for the substrate host.
        std::vector<Worker *> pollWorkers;
        for (auto & worker : workers) {
            if (worker.fromChild) {
                pollfds.push_back({worker.fromChild->fd(), POLLIN, 0});
                pollWorkers.push_back(&worker);
            }
        }
        if (host.usable && host.controlFd >= 0) {
            pollfds.push_back({host.controlFd, POLLIN, 0});
            pollWorkers.push_back(nullptr);
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
            if (worker == nullptr) {
                handleHostMessage();
                if (fatal) {
                    break;
                }
                continue;
            }
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

    // Whatever the interleaved passes did not get to. Most of the walk is
    // usually already done by here, since it ran in the time the coordinator
    // spent waiting on workers.
    while (wantEdges && !edgeDiscovery.done()) {
        stepEdges(1024);
    }

    if (traceEnabled()) {
        fprintf(stderr,
                "trace summary wall=%.3fs workers=%zu startupTotal=%.3fs "
                "warmAttrs=%zu warmAttrTotal=%.3fs meanWarmAttr=%.4fs "
                "jobs=%zu\n",
                traceSecondsSince(trace.start), trace.forks,
                traceSeconds(trace.firstResultTotal), trace.warmAttrs,
                traceSeconds(trace.warmAttrTotal),
                trace.warmAttrs
                    ? traceSeconds(trace.warmAttrTotal) / trace.warmAttrs
                    : 0.0,
                trace.jobs);
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
