#pragma once

#include <string>
#include <vector>

#include "args.hh"

namespace nix {
class EvalState;
struct Value;
class Store;
struct FlakeRef;
template <typename T> class ref;
} // namespace nix

namespace nixcieval {

// Return the flake reference to actually evaluate: when 'shallow' is set and
// 'flakeRef' names a remote git repository, one whose fetch pulls only the
// commit under evaluation (Nix's --depth 1) rather than its whole ancestry.
// Local (file://), github: and gitlab: references, and anything but git, are
// returned unchanged. See the definition for why.
nix::FlakeRef shallowIfRemoteGit(const nix::FlakeRef & flakeRef, bool shallow);

// What evaluating one attribute produced.
enum class OutcomeKind {
    // A buildable derivation was found ('job' fields are populated).
    Job,
    // An attribute set to recurse into ('children' is populated).
    Children,
    // Nothing buildable; ignore this attribute.
    Ignore,
};

struct EvalOutcome {
    OutcomeKind kind = OutcomeKind::Ignore;

    // Populated when kind == Job.
    std::string drvPath;
    // Whether every output is already available (valid locally or
    // substitutable). Always reported.
    bool isCached = false;

    // Populated when kind == Children.
    std::vector<std::string> children;
};

// Owns a single Nix evaluation: opens the store, evaluates the flake once, and
// answers per-attribute evaluation requests. One Evaluator lives in each
// worker process. Evaluation errors are surfaced as thrown exceptions for the
// caller to isolate.
class Evaluator {
  public:
    explicit Evaluator(const Args & args);
    ~Evaluator();

    Evaluator(const Evaluator &) = delete;
    Evaluator & operator=(const Evaluator &) = delete;

    // Evaluate the attribute at 'attrPath' (relative to the discovery root).
    // Throws on evaluation failure.
    EvalOutcome evalAttr(const std::vector<std::string> & attrPath);

    // Force the first derivation the discovery root can reach, so that the
    // very first worker is warm too, before any attribute has been reported
    // and so before there is one to name. It can reach a cheaper corner of
    // the flake than the run goes on to work in, which is what
    // forceSubstrateFor is for afterwards.
    void forceSubstrate();

    // Force everything on the way to 'attrPath', so that a process forked from
    // this one inherits it instead of forcing it again. What lies on the way
    // (nixpkgs, the overlays, the package set the attribute came from) is what
    // an evaluator spends almost all of its startup on, and none of it is
    // specific to the attribute.
    //
    // Forced as far as a worker would, including the attribute's store path,
    // because computing that forces the derivation's inputs and theirs: the
    // package set's derivation graph underneath the attribute, which is what
    // the next attribute mostly reuses. It stops at the cache-status query,
    // which belongs to the attribute alone.
    //
    // The path is one a worker has already reported a derivation for, so it is
    // known to lead somewhere real. Guessing one instead risks forcing a cheap
    // corner of the flake and sharing nothing that the run goes on to need.
    //
    // Best effort: an attribute that throws on the way leaves the evaluator as
    // warm as it got, which is slow rather than wrong.
    void forceSubstrateFor(const std::vector<std::string> & attrPath);

    // Take over the store, after this process was forked from the one that
    // built this evaluator.
    //
    // The evaluator heap survives a fork, which is the point. The store
    // handles must not: a daemon connection is a socket that two processes
    // cannot both speak on, and a local store holds an SQLite connection that
    // SQLite documents as unusable in a forked child. Every way this
    // evaluator reaches a store is pointed at a freshly opened one here.
    void reopenStoreAfterFork();

  private:
    struct Impl;
    Impl * impl_;
};

// One-time process-global Nix initialisation (GC, settings). Call once before
// constructing any Evaluator.
void initNixRuntime(const Args & args);

// Strip ANSI escape sequences from an evaluation error message so it renders
// cleanly in JSON output and logs.
std::string cleanErrorMessage(const std::string & raw);

} // namespace nixcieval
