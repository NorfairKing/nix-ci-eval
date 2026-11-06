#pragma once
// The parts of evaluating with iets (https://github.com/numtide/iets) that
// need neither Nix nor a process, so they can be unit-tested on their own.
//
// iets is only used with import-from-derivation off. It never builds during
// evaluation, and with IFD off Nix refuses exactly the same attributes
// (test/iets/ifd-probe.nix), so it can stand in for Nix there without losing an
// output. docs/iets-evaluator.md has the measurements.

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "pure.hh"

namespace nixcieval {

// A flake reference as Nix parsed it, reduced to what deciding about iets
// needs: the input type, its attributes rendered as strings, and the
// directory of the flake within the input.
struct RootRef {
    std::string type;
    std::map<std::string, std::string> attrs;
    std::string subdir;
};

// A git repository that this tool can check out for iets.
struct GitRoot {
    std::string url;
    // Absent means "whatever the ref, or HEAD, points at", resolved by
    // fetching.
    std::optional<std::string> rev;
    std::optional<std::string> ref;
    std::string subdir;
};

// Why a run is evaluated with Nix even though IFD is off.
struct NotForIets {
    std::string reason;
};

// Whether the root can be handed to iets, and if so what to check out.
//
// iets fetches no root flake, so this tool checks out a git repository for
// it. Anything else, and any git attribute iets would not honour the way Nix
// does (submodules, lfs, a narHash to check against, ...), goes to Nix
// instead: a slower run beats a different one.
std::variant<GitRoot, NotForIets> gitRootFor(const RootRef & root);

// Whether to fetch only the commit under evaluation. Mirrors
// shallowIfRemoteGit: a remote repository is fetched shallow unless the caller
// asked for its history, a local one never is, since nothing it hands over
// crosses a network.
bool fetchShallow(const GitRoot & root, bool shallowAllowed);

// The arguments to `git fetch` in an empty repository that bring down the
// commit to check out. What to fetch is the rev if there is one, otherwise
// the ref, otherwise HEAD.
std::vector<std::string> gitFetchArgs(const GitRoot & root, bool shallow);

// The flake reference iets evaluates: the checkout, pinned to the rev. iets
// accepts only `url`, `ref`, `rev` and `dir` on a git+file root.
std::string ietsFlakeRef(const std::string & checkoutDir,
                         const std::string & rev, const std::string & subdir);

// The --select expression for `iets eval-jobs`: this tool's discovery root,
// walked the way the coordinator walks it.
//
// eval-jobs only enters sets marked recurseForDerivations, and its
// --force-recurse has no depth bound, so a set that contains itself would be
// walked forever. Instead every set that is not a derivation is marked, down
// to the coordinator's depth bound, and past it the set throws. The throw
// arrives as an iets error, which the coordinator hands to Nix, and Nix then
// reports the set as too deep exactly as it always has.
std::string ietsSelectExpr(const std::string & system);

// The arguments to `iets eval-jobs`, without the program name.
struct IetsInvocation {
    std::string flakeRef;
    std::string system;
    std::string storeSocket;
    // Absent leaves the pool size to iets.
    std::optional<std::size_t> workers;
    std::size_t maxMemoryMiB = 4096;
    bool impure = false;
};
std::vector<std::string> ietsArgs(const IetsInvocation & invocation);

// What one line of `iets eval-jobs` output means to the coordinator.
struct IetsJob {
    AttrPath attrPath;
    std::string drvPath;
    bool isCached = false;
};
struct IetsFailed {
    AttrPath attrPath;
};
struct IetsIgnored {};
using IetsLine = std::variant<IetsJob, IetsFailed, IetsIgnored>;

// Read one line of `iets eval-jobs` output. A line with an attribute path and
// an error is a failure, whatever else it carries; a line with an attribute
// path and a drvPath is a job; anything else (a summary, a line without an
// attribute path) is ignored. Throws on a line that is not JSON at all, or
// whose fields have the wrong types.
IetsLine parseIetsLine(const std::string & line);

} // namespace nixcieval
