#include "pure.hh"

#include <cstdint>
#include <set>

namespace nixcieval {

void WorkerCostModel::recordAttr(Duration duration) {
    attrTotal_ += duration;
    attrCount_++;
}

void WorkerCostModel::recordFirstResult(Duration delay) {
    // Everything the worker spent beyond an ordinary attribute is what its
    // existence cost: process startup, loading the flake, and forcing the
    // shared evaluation substrate.
    Duration startup = delay - meanAttr();
    if (startup > startupCost_) {
        startupCost_ = startup;
    }
}

WorkerCostModel::Duration WorkerCostModel::rampInterval() const {
    // Zero until a worker has produced a derivation. A warm evaluation can
    // only be recorded on a worker that already produced one, so there is
    // never a mean attribute time to fall back on while this is unset.
    return startupCost_;
}

WorkerCostModel::Duration WorkerCostModel::meanAttr() const {
    if (attrCount_ == 0) {
        return Duration::zero();
    }
    return attrTotal_ / attrCount_;
}

namespace {

// Whether the pool is still small enough to be justified by what the run has
// watched complete. This is the only brake during the opening moments, before
// any derivation has been timed, so it is deliberately the strict one: a flake
// begins by expanding a handful of attribute sets, which complete in
// milliseconds and say nothing about what evaluating it actually costs.
bool withinEvidenceBound(const PoolSizing & sizing) {
    return sizing.poolSize <= sizing.evaluatedDerivations;
}

} // namespace

bool spawnPendingOnRamp(const PoolSizing & sizing) {
    return sizing.poolSize > 0 && sizing.poolSize < sizing.maxWorkers &&
           sizing.queued > 0 && sizing.idleWorkers == 0 &&
           // Waiting on the evidence bound is not waiting on the clock: only
           // another result can lift it, and that arrives as worker output.
           withinEvidenceBound(sizing);
}

std::chrono::steady_clock::duration
timeUntilSpawnAllowed(const PoolSizing & sizing) {
    auto earliest =
        sizing.rampInterval * static_cast<std::int64_t>(sizing.poolSize);
    if (earliest <= sizing.elapsed) {
        return std::chrono::steady_clock::duration::zero();
    }
    return earliest - sizing.elapsed;
}

bool shouldSpawnWorker(const PoolSizing & sizing) {
    if (sizing.poolSize >= sizing.maxWorkers) {
        return false;
    }
    // Nothing has been forked yet, and it takes a worker to discover the
    // attributes that would justify forking more.
    if (sizing.poolSize == 0) {
        return true;
    }
    if (!spawnPendingOnRamp(sizing)) {
        return false;
    }
    return timeUntilSpawnAllowed(sizing) ==
           std::chrono::steady_clock::duration::zero();
}

std::vector<AttrPath> outputDependenciesFor(
    const AttrPath & attr, const std::string & drvPath,
    const std::map<std::string, std::vector<std::string>> & references,
    const std::map<std::string, AttrPath> & drvPathToAttr) {
    // Depth-first walk of this output's derivation closure, collecting the
    // other discovered outputs reached. The visited set makes it linear in the
    // reachable graph and safe against cycles.
    std::set<AttrPath> depAttrs;
    std::set<std::string> visited{drvPath};
    std::vector<std::string> stack{drvPath};

    while (!stack.empty()) {
        std::string current = std::move(stack.back());
        stack.pop_back();

        auto refsIt = references.find(current);
        if (refsIt == references.end()) {
            continue;
        }
        for (const auto & reference : refsIt->second) {
            if (!visited.insert(reference).second) {
                continue;
            }
            auto attrIt = drvPathToAttr.find(reference);
            if (attrIt != drvPathToAttr.end() && attrIt->second != attr) {
                depAttrs.insert(attrIt->second);
            }
            stack.push_back(reference);
        }
    }

    return std::vector<AttrPath>(depAttrs.begin(), depAttrs.end());
}

std::size_t EdgeDiscovery::intern(const std::string & path) {
    auto found = idByPath_.find(path);
    if (found != idByPath_.end()) {
        return found->second;
    }
    std::size_t id = pathById_.size();
    pathById_.push_back(path);
    idByPath_.emplace(path, id);
    return id;
}

std::vector<OutputEdge> EdgeDiscovery::addOutput(const AttrPath & attr,
                                                 const std::string & drvPath) {
    std::size_t pathId = intern(drvPath);
    std::vector<OutputEdge> edges;

    // Two outputs can be the same derivation, in which case the first to be
    // registered names it. A second name for one derivation would otherwise
    // stand in its own closure and depend on itself.
    if (outputByPathId_.emplace(pathId, outputs_.size()).second) {
        outputs_.push_back(attr);
        std::size_t output = outputs_.size() - 1;

        // Walks that already passed through this derivation were waiting to
        // learn it was an output; they know their edge to it now.
        auto reached = reachedBy_.find(pathId);
        if (reached != reachedBy_.end()) {
            for (std::size_t other : reached->second) {
                if (other != output) {
                    edges.push_back({outputs_[other], attr});
                }
            }
        }

        Walk walk;
        walk.output = output;
        walk.stack.push_back(pathId);
        walk.visited.insert(pathId);
        walks_.push_back(std::move(walk));
        reachedBy_[pathId].push_back(output);

        // As far as what is already known allows. An earlier walk may have
        // been through this derivation and learned its references, and nothing
        // will be queried on this walk's behalf for what is already answered,
        // so a walk left standing here would never move again.
        drain(walks_.back(), edges);
    }
    return edges;
}

// Move one walk as far as it can go, which is until it needs the references of
// a path nobody has answered yet. Leaving that path on top of the stack is what
// nextQuery reads, so a walk is never asked about twice and a shared subgraph
// is queried once rather than once per output that contains it.
void EdgeDiscovery::drain(Walk & walk, std::vector<OutputEdge> & edges) {
    while (!walk.stack.empty()) {
        std::size_t node = walk.stack.back();
        auto known = references_.find(node);
        if (known == references_.end()) {
            return; // blocked until this path is answered
        }
        walk.stack.pop_back();
        for (std::size_t reference : known->second) {
            if (!walk.visited.insert(reference).second) {
                continue;
            }
            reachedBy_[reference].push_back(walk.output);
            auto isOutput = outputByPathId_.find(reference);
            if (isOutput != outputByPathId_.end() &&
                isOutput->second != walk.output) {
                edges.push_back(
                    {outputs_[walk.output], outputs_[isOutput->second]});
            }
            walk.stack.push_back(reference);
        }
    }
}

std::optional<std::string> EdgeDiscovery::nextQuery() {
    for (const auto & walk : walks_) {
        if (walk.stack.empty()) {
            continue;
        }
        std::size_t node = walk.stack.back();
        if (references_.count(node) == 0 && outstanding_.insert(node).second) {
            return pathById_[node];
        }
    }
    return std::nullopt;
}

std::vector<OutputEdge>
EdgeDiscovery::provideReferences(const std::string & path,
                                 const std::vector<std::string> & references) {
    std::size_t pathId = intern(path);
    outstanding_.erase(pathId);
    std::vector<std::size_t> referenceIds;
    referenceIds.reserve(references.size());
    for (const auto & reference : references) {
        referenceIds.push_back(intern(reference));
    }
    references_.emplace(pathId, referenceIds);

    std::vector<OutputEdge> edges;
    for (auto & walk : walks_) {
        drain(walk, edges);
    }
    return edges;
}

bool EdgeDiscovery::done() const {
    for (const auto & walk : walks_) {
        if (!walk.stack.empty()) {
            return false;
        }
    }
    return true;
}

std::size_t residentMiBFromStatm(const std::string & statm,
                                 std::size_t pageSizeBytes) {
    // "size resident shared text lib data dt", in pages. The second field is
    // the one that answers how much this process is holding right now.
    std::size_t cursor = statm.find_first_not_of(" \t");
    if (cursor == std::string::npos) {
        return 0;
    }
    std::size_t firstEnd = statm.find_first_of(" \t", cursor);
    if (firstEnd == std::string::npos) {
        return 0;
    }
    std::size_t secondStart = statm.find_first_not_of(" \t", firstEnd);
    if (secondStart == std::string::npos) {
        return 0;
    }
    std::size_t pages = 0;
    bool anyDigits = false;
    for (std::size_t i = secondStart; i < statm.size(); i++) {
        char c = statm[i];
        if (c < '0' || c > '9') {
            break;
        }
        anyDigits = true;
        pages = pages * 10 + static_cast<std::size_t>(c - '0');
    }
    if (!anyDigits) {
        return 0;
    }
    auto bytes = static_cast<std::uint64_t>(pages) *
                 static_cast<std::uint64_t>(pageSizeBytes);
    return static_cast<std::size_t>(bytes / (1024ULL * 1024ULL));
}

bool shouldRetryAfterWorkerDeath(int attemptsSoFar) {
    // Two deaths is enough to tell the two cases apart: an attribute that
    // kills whatever evaluates it will have done so twice, while a worker lost
    // to something ambient is very unlikely to be lost twice in a row on the
    // same attribute.
    constexpr int maxAttempts = 2;
    return attemptsSoFar < maxAttempts;
}

std::string truncateForLine(const std::string & message, std::size_t maxBytes) {
    if (message.size() <= maxBytes) {
        return message;
    }
    static const std::string marker = "… (truncated)";
    return message.substr(0, maxBytes) + marker;
}

} // namespace nixcieval
