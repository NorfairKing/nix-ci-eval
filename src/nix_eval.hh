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
