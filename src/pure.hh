#pragma once
// Pure helpers with no dependency on the Nix libraries, so they can be
// unit-tested on their own (see test/pure_test.cc).

#include <chrono>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace nixcieval {

// What a run has learned about the cost of evaluating this flake, used to size
// the worker pool.
//
// A bigger pool is not always better, and how much better depends on what a
// worker costs to start. One forked from a warm evaluator inherits the flake
// and everything its outputs share, and costs almost nothing; one that has to
// load the flake and force all of that itself costs the run that whole
// evaluation, and past the point where the pool can cover the backlog, extra
// workers of that kind buy no wall-clock time at all.
//
// Which kind a run gets is not assumed: both quantities that decide the
// trade-off are measured as the run proceeds, so the pool grows freely when
// workers are cheap and cautiously when they are not.
class WorkerCostModel {
  public:
    using Duration = std::chrono::steady_clock::duration;

    // Record how long a derivation took to evaluate on a worker that had
    // already evaluated one. Only these are timed: recursing into an attribute
    // set is structural rather than real work, and a worker's first derivation
    // carries the startup cost this model exists to expose.
    void recordAttr(Duration duration);

    // Record how long a freshly forked worker took to produce its first
    // derivation, measured from the fork.
    void recordFirstResult(Duration delay);

    // What one more worker is estimated to cost, which is the run time each
    // worker in the pool has to justify (see shouldSpawnWorker). Zero until
    // something has been measured, which lets the pool make its opening moves
    // without waiting on an estimate it does not have yet.
    Duration rampInterval() const;

  private:
    // The mean time an already-warm worker takes over a derivation, or zero if
    // none has been recorded yet.
    Duration meanAttr() const;

    Duration attrTotal_{};
    std::size_t attrCount_ = 0;
    // The largest per-worker startup cost seen so far. Taking the maximum
    // makes the pool grow conservatively: over-estimating a worker's cost only
    // slows growth, while under-estimating it re-creates the oversized pool
    // this model exists to avoid.
    Duration startupCost_{};
};

// The state a decision to grow the worker pool is made from.
struct PoolSizing {
    // Attributes waiting for a worker.
    std::size_t queued = 0;
    // Workers forked so far, whether idle, busy, or still starting up.
    std::size_t poolSize = 0;
    // Workers that could take an attribute right now.
    std::size_t idleWorkers = 0;
    // Attributes that evaluated to a derivation.
    //
    // Only these count as evidence that the pool is worth growing. Counting
    // every result instead would count the walk of the attribute tree, where
    // each step is a symbol lookup costing almost nothing, and a large flake
    // produces thousands of those before the first derivation. The pool would
    // then reach its ceiling before a single derivation had been evaluated or
    // timed, which is the all-at-once startup the ramp exists to prevent.
    std::size_t evaluatedDerivations = 0;
    // The ceiling on the pool, from --workers or the CPU count.
    std::size_t maxWorkers = 1;
    // How long the run has been going.
    std::chrono::steady_clock::duration elapsed{};
    // What one more worker is estimated to cost, from WorkerCostModel.
    std::chrono::steady_clock::duration rampInterval{};
};

// Whether the pool has a reason to grow and is held back only by the passage
// of time, meaning it will grow once the run has lasted long enough. Lets a
// caller wait exactly that long rather than polling for the moment, so a pool
// held back for any other reason keeps waiting on its workers instead.
bool spawnPendingOnRamp(const PoolSizing & sizing);

// When the run will have lasted long enough for the pool to grow again, as a
// duration from now. Zero once growing is already allowed. Only meaningful
// while spawnPendingOnRamp holds.
std::chrono::steady_clock::duration
timeUntilSpawnAllowed(const PoolSizing & sizing);

// Whether to fork one more evaluation worker.
//
// The pool grows only while attributes are waiting that no existing worker can
// take, and only while the run has already lasted longer than the workers so
// far have cost to start. Keeping the pool under 'elapsed / rampInterval'
// spends at most about half the run's time on worker startup no matter how
// deep the backlog gets, and it corrects itself: an estimate that starts out
// too cheap and rises as real attributes are measured simply stops the pool
// growing further, rather than leaving it permanently oversized.
//
// Until an evaluation has been timed there is no such estimate, so the pool is
// held to the attributes the run has already seen through. That is what keeps
// a run from committing to a large pool in the moments before it has any idea
// what this flake costs.
bool shouldSpawnWorker(const PoolSizing & sizing);

// An attribute path, the same shape as a job's "attrPath": one component per
// level, so it never has to be parsed back out of a rendered string.
using AttrPath = std::vector<std::string>;

// The dependencies of one discovered output: the other discovered outputs
// reached from its derivation, sorted, de-duplicated and excluding itself.
// Empty when it depends on none of them.
//
// 'references' is the derivation reference graph: each derivation path mapped
// to its direct reference derivation paths (input derivations).
// 'drvPathToAttr' maps every discovered output's derivation path to its
// attribute path; its keys are the set of output derivations to look for in
// this output's closure.
//
// The derivation's transitive closure over 'references' is walked, and the
// other discovered outputs found in it become the dependencies. The walk is
// cycle-safe, and its cost is linear in the reachable graph rather than
// re-deriving the closure from the store.
//
// One output at a time, so a caller can emit each output's edges as they are
// computed instead of accumulating every output's first.
std::vector<AttrPath> outputDependenciesFor(
    const AttrPath & attr, const std::string & drvPath,
    const std::map<std::string, std::vector<std::string>> & references,
    const std::map<std::string, AttrPath> & drvPathToAttr);

// One dependency edge: the output that depends, and the one it depends on.
struct OutputEdge {
    AttrPath from;
    AttrPath to;
};

// Works out which discovered outputs lie in which others' closures, reporting
// each edge as soon as both of its ends are known rather than once an output
// has been walked in full.
//
// An edge is final the moment both of its ends are known, so waiting for the
// rest of an output's closure only delays telling someone a thing that cannot
// change. The two ends can be learned in either order, and both are handled:
// walking A may reach B's derivation before B has been discovered, or B may be
// discovered after several walks have already passed through it.
//
// Knows nothing about a store. It names the path whose references it needs and
// is told the answer, so the traversal can be driven from the coordinator's
// idle time and tested without a store at all.
class EdgeDiscovery {
  public:
    // Register a discovered output. Returns the edges that this makes known,
    // which are those from outputs whose walks already passed through this
    // output's derivation.
    std::vector<OutputEdge> addOutput(const AttrPath & attr,
                                      const std::string & drvPath);

    // The next path whose references are needed, or nothing when every walk is
    // waiting on nothing.
    std::optional<std::string> nextQuery();

    // Answer the path nextQuery named. Returns the edges this answer makes
    // known.
    std::vector<OutputEdge>
    provideReferences(const std::string & path,
                      const std::vector<std::string> & references);

    // Whether every registered output has been walked to exhaustion.
    bool done() const;

  private:
    // One walk per output: what it still has to expand, and what it has seen.
    struct Walk {
        std::size_t output = 0;
        std::vector<std::size_t> stack;
        std::set<std::size_t> visited;
    };

    void drain(Walk & walk, std::vector<OutputEdge> & edges);

    // Paths are interned so that the per-output visited sets, which together
    // hold one entry per output per node of its closure, cost an integer each
    // rather than a string each.
    std::size_t intern(const std::string & path);

    std::vector<std::string> pathById_;
    std::map<std::string, std::size_t> idByPath_;

    std::vector<AttrPath> outputs_;
    std::map<std::size_t, std::size_t> outputByPathId_;

    // Which outputs' walks have reached a given path, so that an output
    // discovered later can be joined up to the walks that already passed it.
    std::map<std::size_t, std::vector<std::size_t>> reachedBy_;

    std::vector<Walk> walks_;

    // Paths handed out by nextQuery and not yet answered, so the same path is
    // not queried once per walk that wants it.
    std::set<std::size_t> outstanding_;
    std::map<std::size_t, std::vector<std::size_t>> references_;
};

// The resident size in MiB reported by the contents of /proc/self/statm, whose
// second field is the resident page count.
//
// This is what a worker is using now, deliberately not the lifetime peak that
// getrusage reports: a peak never falls, so one expensive attribute would put
// a worker permanently over the memory ceiling and force it to be restarted
// after every subsequent attribute for the rest of the run.
//
// Returns 0 if the contents do not have the expected shape, so an unreadable
// or unexpected /proc leaves the ceiling un-triggered rather than recycling
// every worker constantly.
std::size_t residentMiBFromStatm(const std::string & statm,
                                 std::size_t pageSizeBytes);

// The resident size in MiB that this process does not share with any other,
// from the contents of /proc/self/smaps_rollup: its Private_Clean and
// Private_Dirty lines, which are reported in KiB.
//
// This is what a worker costs on top of the evaluator it inherited from the
// process that forked it. Total resident size would count that inherited
// evaluator against every worker sharing it, so a pool that shares one 2 GiB
// evaluator would have every worker over a 2 GiB ceiling while between them
// they held 2 GiB once.
//
// Returns nothing when the contents do not have the expected shape, so that a
// kernel without smaps_rollup falls back to the resident figure rather than
// reading zero and never recycling a worker at all.
std::optional<std::size_t>
privateMiBFromSmapsRollup(const std::string & rollup);

// The number of threads in a process, from the contents of
// /proc/<pid>/status.
//
// A forked child inherits the address space of its parent but none of its
// threads, so anything that answers on a thread of its own answers nothing in
// the child. Nix starts one to download with, and a worker that inherited it
// would wait forever on its first fetch. Watching this number is how a run
// notices that its evaluator has become something no worker may inherit.
//
// Returns nothing when the contents do not have the expected shape, which a
// caller reads as the unsafe answer rather than a count of zero.
std::optional<std::size_t> threadsFromProcStatus(const std::string & status);

// Shorten a message that is about to be put on a line, so that a flake cannot
// make one arbitrarily large. A message longer than 'maxBytes' is cut and
// marked, rather than dropped, so the reason a job failed survives.
std::string truncateForLine(const std::string & message, std::size_t maxBytes);

// Whether an attribute whose worker died holding it is worth handing to
// another worker, given how many have already died on it.
//
// A worker dies for reasons that have nothing to do with the attribute: the
// kernel's OOM killer picks a process, not a guilty one. Reporting the
// attribute as failing to evaluate on the strength of that would put a bogus
// error in the output, and a different one on every run. Retrying on a fresh
// worker separates "this attribute is broken", which fails every time, from
// "this worker died", which does not. The bound is what stops an attribute
// that really does kill workers from doing it forever.
bool shouldRetryAfterWorkerDeath(int attemptsSoFar);

} // namespace nixcieval
