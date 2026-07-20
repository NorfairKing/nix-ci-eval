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
