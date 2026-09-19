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
#include <nix/util/canon-path.hh>
#include <nix/util/configuration.hh>
#include <nix/util/current-process.hh>
#include <nix/util/mounted-source-accessor.hh>
#include <nix/util/source-accessor.hh>
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

// How far from the discovery root the first-derivation walk may go, and how
// many attributes it may visit. A real output sits three levels down
// (packages.<system>.<name>) and a flake that groups its outputs nests a few
// more; past that the walk is not going to find a derivation it was going to
// find.
constexpr std::size_t kSubstrateWalkDepth = 6;
constexpr std::size_t kSubstrateWalkBudget = 256;

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

// Lock the flake this run is to discover, which is the step that fetches it
// and its inputs.
//
// Read-only: never update, write, or fetch from registries, and require the
// flake to be fully locked.
nix::flake::LockedFlake lockFlakeForDiscovery(nix::EvalState & state,
                                              const Args & args) {
    auto flakeRef =
        shallowIfRemoteGit(nix::parseFlakeRef(nix::fetchSettings, args.flakeRef,
                                              std::filesystem::current_path()),
                           args.shallow);
    nix::flake::LockFlags lockFlags;
    lockFlags.updateLockFile = false;
    lockFlags.writeLockFile = false;
    lockFlags.useRegistries = false;
    lockFlags.allowUnlocked = false;
    return nix::flake::lockFlake(nix::flakeSettings, state, flakeRef,
                                 lockFlags);
}

struct Evaluator::Impl {
    Args args;
    nix::ref<nix::Store> store;
    // Stores this evaluator no longer uses but must not destroy: the one
    // inherited across a fork is still in use by the process that was forked,
    // whose handles its destructor would act on.
    std::vector<nix::ref<nix::Store>> retiredStores;
    nix::ref<nix::EvalState> state;
    nix::Value * root;

    explicit Impl(const Args & a)
        : args(a), store(nix::openStore()),
          state(nix::make_ref<nix::EvalState>(
              nix::LookupPath{}, store, nix::fetchSettings, nix::evalSettings)),
          root(nullptr) {
        auto locked = lockFlakeForDiscovery(*state, args);
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

    // Force one attribute as far as a worker would, short of asking the
    // store what is already built.
    //
    // Navigating forces every level on the way, which is where one kind of
    // shared work lives: reaching a package forces the package set it came
    // from. The store path is where the other kind lives, and it is the
    // larger one: computing it forces the derivation's inputs, and theirs,
    // which is the package set's derivation graph underneath this attribute
    // and is almost entirely shared with the next attribute.
    //
    // What it stops short of is the cache-status query, which is this
    // attribute's alone and, without a daemon to ask, would put a file
    // transfer and its background thread in a process that is about to fork.
    void forceAsAWorkerWould(nix::Value & value) {
        auto packageInfo = nix::getDerivation(*state, value, false);
        if (!packageInfo) {
            return;
        }
        packageInfo->requireDrvPath();
    }

    // Force the first derivation reachable from the root, depth first and in
    // lexicographic order, so that the very first worker is warm too. Bounded
    // in both directions: past a few levels, or a couple of hundred
    // attributes, this is not going to find a derivation it was going to
    // find.
    bool forceFirstDerivation(nix::Value & value, std::size_t depth,
                              std::size_t & budget) {
        if (budget == 0 || depth > kSubstrateWalkDepth) {
            return false;
        }
        budget--;
        state->forceValue(value, nix::noPos);
        if (value.type() != nix::nAttrs) {
            return false;
        }
        if (nix::getDerivation(*state, value, false)) {
            forceAsAWorkerWould(value);
            return true;
        }
        for (const auto & attr :
             value.attrs()->lexicographicOrder(state->symbols)) {
            try {
                if (forceFirstDerivation(*attr->value, depth + 1, budget)) {
                    return true;
                }
            } catch (const std::exception &) {
                // An attribute that throws is one this walk steps over: it is
                // here to warm the evaluator, and the worker that is handed
                // that attribute is what reports the failure.
            }
        }
        return false;
    }

    void forceSubstrate() {
        std::size_t budget = kSubstrateWalkBudget;
        try {
            forceFirstDerivation(*root, 0, budget);
        } catch (const std::exception &) {
        }
    }

    void forceSubstrateFor(const std::vector<std::string> & attrPath) {
        try {
            nix::Value * value = navigate(attrPath);
            if (value == nullptr) {
                return;
            }
            forceAsAWorkerWould(*value);
        } catch (const std::exception &) {
            // An attribute that cannot be evaluated leaves the evaluator as
            // warm as it got. The worker that is handed it reports the
            // failure; this is not the place to.
        }
    }

    void reopenStoreAfterFork() {
        auto fresh = nix::openStore();

        retiredStores.push_back(store);
        store = fresh;

        // Reading a store path during evaluation goes through this
        // accessor, which checks the path against the store it was made
        // from. Left alone it would send every such check to the inherited
        // store.
        state->storeFS->mount(nix::CanonPath(fresh->storeDir),
                              fresh->getFSAccessor(nix::evalSettings.pureEval));

        // EvalState declares the stores it writes derivations through
        // const, and nix offers no supported way to replace them, so this
        // writes through that const. The alternative is to let every forked
        // worker share one daemon connection, which is a socket they would
        // interleave on, or one SQLite connection, which they would
        // corrupt.
        const_cast<nix::ref<nix::Store> &>(state->store) = fresh;
        const_cast<nix::ref<nix::Store> &>(state->buildStore) = fresh;
    }

    // Navigate from the root to an attribute path, forcing each level.
    // Returns nullptr when a path component is missing.
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
    // deliberately not across jobs: each job's cache status is reported
    // inline as it is discovered (the streaming contract), and cross-job
    // batching would require buffering every job first. Cache status is
    // already computed in parallel across the worker processes.
    bool queryIsCached(const nix::PackageInfo::Outputs & outputs) {
        nix::StorePathCAMap toQuery;
        for (const auto & [outputName, optPath] : outputs) {
            if (!optPath) {
                // A content-addressed output with no static path: cannot
                // tell.
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

void Evaluator::forceSubstrate() { impl_->forceSubstrate(); }

void Evaluator::forceSubstrateFor(const std::vector<std::string> & attrPath) {
    impl_->forceSubstrateFor(attrPath);
}

void Evaluator::reopenStoreAfterFork() { impl_->reopenStoreAfterFork(); }

void initNixRuntime(const Args & args) {
    // Evaluate with the same stack the `nix` CLI gives itself. A worker
    // inherits the default 8 MiB stack, but a deep-but-finite flake (nested
    // attribute sets, a large data literal, a value materialised from an
    // IFD) recurses far enough in the evaluator to overflow that, and Nix
    // turns a stack overflow into an uncatchable SIGSEGV: the worker dies
    // and its attribute is reported as a crashed worker rather than
    // discovered. The `nix` CLI raises the stack to 64 MiB before
    // evaluating for exactly this reason, so `nix flake show` handled such
    // flakes; matching it keeps discovery from regressing against what a
    // caller could evaluate by hand. Raised here in the coordinator, before
    // any worker is forked, so every worker inherits the larger limit.
    nix::setStackSize(64 * 1024 * 1024);

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
    // during evaluation; discovery never builds, so disable remote
    // builders.
    nix::settings.getWorkerSettings().builders = "";

    if (args.impure) {
        nix::evalSettings.pureEval = false;
    } else {
        nix::evalSettings.pureEval = true;
    }

    // Discovery allows import-from-derivation by default: an output that
    // only resolves through IFD would otherwise be missed. A caller that
    // wants to forbid it (e.g. a suite that opts out) passes
    // --no-import-from-derivation.
    nix::evalSettings.enableImportFromDerivation = args.importFromDerivation;

    // Open the fetcher cache once in the parent so forked workers do not
    // race to create it.
    nix::fetchSettings.getCache();
}

std::string cleanErrorMessage(const std::string & raw) {
    return nix::filterANSIEscapes(raw, true);
}

} // namespace nixcieval
