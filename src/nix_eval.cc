#include "nix_eval.hh"

#include <filesystem>
#include <stdexcept>

#include <nix/cmd/common-eval-args.hh>
#include <nix/expr/attr-set.hh>
#include <nix/expr/eval-gc.hh>
#include <nix/expr/eval-settings.hh>
#include <nix/expr/eval.hh>
#include <nix/expr/get-drvs.hh>
#include <nix/expr/value.hh>
#include <nix/fetchers/attrs.hh>
#include <nix/fetchers/fetch-settings.hh>
#include <nix/fetchers/fetchers.hh>
#include <nix/flake/flake.hh>
#include <nix/flake/flakeref.hh>
#include <nix/flake/settings.hh>
#include <nix/main/shared.hh>
#include <nix/store/derivations.hh>
#include <nix/store/globals.hh>
#include <nix/store/store-api.hh>
#include <nix/store/store-open.hh>
#include <nix/util/configuration.hh>
#include <nix/util/terminal.hh>

#include "pure.hh"

namespace nixcieval {

namespace {

// Render a string as a Nix double-quoted string literal.
std::string nixStringLiteral(const std::string & value) {
    std::string out = "\"";
    for (char c : value) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '$':
            out += "\\$";
            break;
        default:
            out += c;
        }
    }
    out += "\"";
    return out;
}

// The discovery root: gather every supported output type for the target system
// into one attribute set. Output types the flake does not define become {} (via
// `or {}`), which simply recurse into nothing.
//
// The system is kept as its own level rather than collapsed away, so that an
// attribute path reads exactly as it does in the flake
// (packages.<system>.<name>) and can be handed to `nix build` unchanged. The
// alternative, reporting packages.<name> and splicing the system back in, makes
// every consumer reconstruct a path this already knows.
std::string selectExpr(const std::string & system) {
    std::string sys = nixStringLiteral(system);
    return "flake: {"
           "  packages." +
           sys + " = flake.packages." + sys +
           " or {};"
           "  checks." +
           sys + " = flake.checks." + sys +
           " or {};"
           "  devShells." +
           sys + " = flake.devShells." + sys +
           " or {};"
           "  formatter." +
           sys + " = flake.formatter." + sys +
           " or {};"
           "  devShell." +
           sys + " = flake.devShell." + sys +
           " or {};"
           "}";
}

} // namespace

// Fetching a git flake ref pulls down every commit the requested one descends
// from, which for a long-lived repository is orders of magnitude more than the
// single tree this tool evaluates. Asking for a shallow fetch makes Nix pass
// --depth 1, so only the commit under evaluation crosses the network.
//
// The cost is that Nix cannot count revisions without the history, so it leaves
// out `revCount` and a flake reading self.revCount fails to evaluate. That is
// what --no-shallow is for.
//
// Local repositories are left alone, because nothing they hand over crosses a
// network: shallowness would win them little and cost them revCount all the
// same.
nix::FlakeRef shallowIfRemoteGit(const nix::FlakeRef & flakeRef, bool shallow) {
    if (!shallow) {
        return flakeRef;
    }
    if (flakeRef.input.getType() != "git") {
        return flakeRef;
    }
    auto url = nix::fetchers::maybeGetStrAttr(flakeRef.input.attrs, "url");
    if (!url || url->starts_with("file:")) {
        return flakeRef;
    }
    auto attrs = flakeRef.input.attrs;
    attrs.insert_or_assign("shallow", nix::Explicit<bool>{true});
    return nix::FlakeRef(
        nix::fetchers::Input::fromAttrs(nix::fetchSettings, std::move(attrs)),
        flakeRef.subdir);
}

struct Evaluator::Impl {
    Args args;
    nix::ref<nix::Store> store;
    nix::ref<nix::EvalState> state;
    nix::Value * root;

    explicit Impl(const Args & a)
        : args(a), store(nix::openStore()),
          state(nix::make_ref<nix::EvalState>(
              nix::LookupPath{}, store, nix::fetchSettings, nix::evalSettings)),
          root(nullptr) {
        auto flakeRef = shallowIfRemoteGit(
            nix::parseFlakeRef(nix::fetchSettings, args.flakeRef,
                               std::filesystem::current_path()),
            args.shallow);
        // Read-only locking: never update, write, or fetch from registries,
        // and require the flake to be fully locked.
        nix::flake::LockFlags lockFlags;
        lockFlags.updateLockFile = false;
        lockFlags.writeLockFile = false;
        lockFlags.useRegistries = false;
        lockFlags.allowUnlocked = false;
        auto locked = nix::flake::lockFlake(nix::flakeSettings, *state,
                                            flakeRef, lockFlags);
        auto * flakeValue = state->allocValue();
        nix::flake::callFlake(*state, locked, *flakeValue);

        auto * expr = state->parseExprFromString(selectExpr(args.system),
                                                 state->rootPath("."));
        nix::Value selectFun;
        state->eval(expr, selectFun);
        root = state->allocValue();
        state->callFunction(selectFun, *flakeValue, *root, nix::noPos);
        state->forceAttrs(*root, nix::noPos, "the discovery root");
    }

    // Navigate from the root to an attribute path, forcing each level. Returns
    // nullptr when a path component is missing.
    nix::Value * navigate(const std::vector<std::string> & attrPath) {
        nix::Value * current = root;
        for (const auto & component : attrPath) {
            state->forceValue(*current, nix::noPos);
            if (current->type() != nix::nAttrs) {
                return nullptr;
            }
            const auto * attr =
                current->attrs()->get(state->symbols.create(component));
            if (attr == nullptr) {
                return nullptr;
            }
            current = attr->value;
        }
        state->forceValue(*current, nix::noPos);
        return current;
    }

    // Whether every output of the derivation is already available, either
    // valid in the local store or substitutable from a configured cache.
    //
    // The substituter probe is batched across this derivation's outputs but
    // deliberately not across jobs: each job's cache status is reported inline
    // as it is discovered (the streaming contract), and cross-job batching
    // would require buffering every job first. Cache status is already computed
    // in parallel across the worker processes.
    bool queryIsCached(const nix::PackageInfo::Outputs & outputs) {
        nix::StorePathCAMap toQuery;
        for (const auto & [outputName, optPath] : outputs) {
            if (!optPath) {
                // A content-addressed output with no static path: cannot tell.
                return false;
            }
            if (!store->isValidPath(*optPath)) {
                toQuery.insert({*optPath, std::nullopt});
            }
        }
        if (toQuery.empty()) {
            return true;
        }
        nix::SubstitutablePathInfos infos;
        store->querySubstitutablePathInfos(toQuery, infos);
        return infos.size() == toQuery.size();
    }

    EvalOutcome evalAttr(const std::vector<std::string> & attrPath) {
        nix::Value * value = navigate(attrPath);
        if (value == nullptr) {
            EvalOutcome ignore;
            ignore.kind = OutcomeKind::Ignore;
            return ignore;
        }

        auto packageInfo = nix::getDerivation(*state, *value, false);
        if (!packageInfo) {
            if (value->type() != nix::nAttrs) {
                EvalOutcome ignore;
                ignore.kind = OutcomeKind::Ignore;
                return ignore;
            }
            EvalOutcome outcome;
            outcome.kind = OutcomeKind::Children;
            for (const auto & attr :
                 value->attrs()->lexicographicOrder(state->symbols)) {
                outcome.children.emplace_back(state->symbols[attr->name]);
            }
            return outcome;
        }

        EvalOutcome outcome;
        outcome.kind = OutcomeKind::Job;
        outcome.drvPath = store->printStorePath(packageInfo->requireDrvPath());
        outcome.isCached = queryIsCached(packageInfo->queryOutputs(true));

        return outcome;
    }
};

Evaluator::Evaluator(const Args & args) : impl_(new Impl(args)) {}

Evaluator::~Evaluator() { delete impl_; }

EvalOutcome Evaluator::evalAttr(const std::vector<std::string> & attrPath) {
    return impl_->evalAttr(attrPath);
}

void initNixRuntime(const Args & args) {
    nix::initNix();
    nix::initGC();

    // This tool always evaluates flakes, so enable the features it needs
    // regardless of the ambient Nix configuration (a worker's managed
    // environment may not enable them). Appended, so ambient features (e.g.
    // ca-derivations) are preserved.
    nix::experimentalFeatureSettings.experimentalFeatures.set(
        "flakes nix-command", true);

    nix::flakeSettings.configureEvalSettings(nix::evalSettings);

    // The build hook plus import-from-derivation can cause "unexpected EOF"
    // during evaluation; discovery never builds, so disable remote builders.
    nix::settings.getWorkerSettings().builders = "";

    if (args.impure) {
        nix::evalSettings.pureEval = false;
    } else {
        nix::evalSettings.pureEval = true;
    }

    // Discovery allows import-from-derivation by default: an output that only
    // resolves through IFD would otherwise be missed. A caller that wants to
    // forbid it (e.g. a suite that opts out) passes
    // --no-import-from-derivation.
    nix::evalSettings.enableImportFromDerivation = args.importFromDerivation;

    // Open the fetcher cache once in the parent so forked workers do not race
    // to create it.
    nix::fetchSettings.getCache();
}

std::string cleanErrorMessage(const std::string & raw) {
    return nix::filterANSIEscapes(raw, true);
}

} // namespace nixcieval
