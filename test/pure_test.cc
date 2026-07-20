// Minimal dependency-free unit tests for the pure helpers. Exits non-zero on
// the first failed assertion so `meson test` reports it.

#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "args.hh"
#include "pure.hh"

namespace {

int failures = 0;

// Parse a command line the way main() would, minus the program name.
nixcieval::Args parse(const std::vector<std::string> & arguments) {
    std::vector<char *> argv;
    std::string program = "nix-ci-eval";
    argv.push_back(program.data());
    std::vector<std::string> owned = arguments;
    for (auto & argument : owned) {
        argv.push_back(argument.data());
    }
    return nixcieval::parseArgs(static_cast<int>(argv.size()), argv.data());
}

// Whether parsing these arguments was rejected.
bool rejects(const std::vector<std::string> & arguments) {
    try {
        parse(arguments);
        return false;
    } catch (const nixcieval::HelpRequested &) {
        return false;
    } catch (const std::exception &) {
        return true;
    }
}

// Whether these arguments asked for the usage text, which is an answer rather
// than a mistake and so must not travel as an error.
bool asksForHelp(const std::vector<std::string> & arguments) {
    try {
        parse(arguments);
        return false;
    } catch (const nixcieval::HelpRequested & help) {
        return !help.usage.empty();
    } catch (const std::exception &) {
        return false;
    }
}

void expectEq(const std::string & what, const std::string & got,
              const std::string & want) {
    if (got != want) {
        failures++;
        fprintf(stderr, "FAIL %s: got \"%s\", want \"%s\"\n", what.c_str(),
                got.c_str(), want.c_str());
    }
}

void expectBool(const std::string & what, bool got, bool want) {
    if (got != want) {
        failures++;
        fprintf(stderr, "FAIL %s: got %s, want %s\n", what.c_str(),
                got ? "true" : "false", want ? "true" : "false");
    }
}

// Render an attribute path with dots, so the assertions below stay readable.
std::string renderPath(const nixcieval::AttrPath & path) {
    std::string out;
    for (const auto & component : path) {
        if (!out.empty()) {
            out += ".";
        }
        out += component;
    }
    return out;
}

// Walk every output the way the coordinator streams them and render the edges
// as "attr:dep,dep;attr:dep" for exact assertions. Outputs with no
// dependencies produce nothing, as they emit no line.
std::string renderDeps(
    const std::vector<std::pair<nixcieval::AttrPath, std::string>> & outputs,
    const std::map<std::string, std::vector<std::string>> & references,
    const std::map<std::string, nixcieval::AttrPath> & drvPathToAttr) {
    std::string out;
    for (const auto & [attr, drvPath] : outputs) {
        auto deps = nixcieval::outputDependenciesFor(attr, drvPath, references,
                                                     drvPathToAttr);
        if (deps.empty()) {
            continue;
        }
        if (!out.empty()) {
            out += ";";
        }
        std::string joined;
        for (const auto & dep : deps) {
            if (!joined.empty()) {
                joined += ",";
            }
            joined += renderPath(dep);
        }
        out += renderPath(attr) + ":" + joined;
    }
    return out;
}

} // namespace

int main() {
    using namespace nixcieval;

    {
        // /proc/self/statm is "size resident shared ..." in pages, and it is
        // the second field that says what is held right now.
        expectBool("resident pages are read from the second field",
                   residentMiBFromStatm("100 512 7 1 0 9 0", 4096) == 2, true);
        expectBool("a short page is scaled by the page size",
                   residentMiBFromStatm("100 256 7", 4096) == 1, true);
        expectBool("leading whitespace is tolerated",
                   residentMiBFromStatm("  100 512 7", 4096) == 2, true);
        // An unreadable or unexpected /proc must leave the ceiling alone
        // rather than recycle every worker after every attribute.
        expectBool("empty contents read as zero",
                   residentMiBFromStatm("", 4096) == 0, true);
        expectBool("a single field reads as zero",
                   residentMiBFromStatm("100", 4096) == 0, true);
        expectBool("a non-numeric second field reads as zero",
                   residentMiBFromStatm("100 abc 7", 4096) == 0, true);
    }

    {
        // A worker dying is not evidence that the attribute it held is
        // broken, so the attribute goes back for another worker; an attribute
        // that keeps killing workers eventually stops.
        expectBool("a first death is retried", shouldRetryAfterWorkerDeath(0),
                   true);
        expectBool("a second death is retried", shouldRetryAfterWorkerDeath(1),
                   true);
        expectBool("a third death is not", shouldRetryAfterWorkerDeath(2),
                   false);
        expectBool("further deaths are not", shouldRetryAfterWorkerDeath(9),
                   false);
    }

    {
        expectEq("a short message is left alone", truncateForLine("hello", 64),
                 "hello");
        expectEq("a message exactly at the limit is left alone",
                 truncateForLine("12345", 5), "12345");
        // The reason has to survive; only the flood is cut.
        expectEq("an oversized message is cut and marked",
                 truncateForLine("123456789", 4), "1234… (truncated)");
    }

    {
        // A depends directly on hello, which is also a discovered output; the
        // unrelated node "zzz" and hello's own (non-output) input are ignored.
        std::vector<std::pair<AttrPath, std::string>> outputs = {
            {AttrPath{"packages", "hello"}, "/nix/store/aaa.drv"},
            {AttrPath{"packages", "world"}, "/nix/store/bbb.drv"},
            {AttrPath{"devShells", "default"}, "/nix/store/ccc.drv"},
        };
        std::map<std::string, AttrPath> drvToAttr = {
            {"/nix/store/aaa.drv", AttrPath{"packages", "hello"}},
            {"/nix/store/bbb.drv", AttrPath{"packages", "world"}},
            {"/nix/store/ccc.drv", AttrPath{"devShells", "default"}},
        };
        std::map<std::string, std::vector<std::string>> references = {
            {"/nix/store/ccc.drv",
             {"/nix/store/aaa.drv", "/nix/store/zzz.drv"}},
            {"/nix/store/aaa.drv", {"/nix/store/lib.drv"}},
            {"/nix/store/bbb.drv", {}},
        };
        expectEq("direct dependency, non-outputs ignored",
                 renderDeps(outputs, references, drvToAttr),
                 "devShells.default:packages.hello");
    }
    {
        // a -> b -> c: a depends on b (direct) and c (transitive); b on c.
        std::vector<std::pair<AttrPath, std::string>> outputs = {
            {AttrPath{"a"}, "/nix/store/da.drv"},
            {AttrPath{"b"}, "/nix/store/db.drv"},
            {AttrPath{"c"}, "/nix/store/dc.drv"},
        };
        std::map<std::string, AttrPath> drvToAttr = {
            {"/nix/store/da.drv", AttrPath{"a"}},
            {"/nix/store/db.drv", AttrPath{"b"}},
            {"/nix/store/dc.drv", AttrPath{"c"}},
        };
        std::map<std::string, std::vector<std::string>> references = {
            {"/nix/store/da.drv", {"/nix/store/db.drv"}},
            {"/nix/store/db.drv", {"/nix/store/dc.drv"}},
            {"/nix/store/dc.drv", {}},
        };
        expectEq("transitive dependencies, sorted",
                 renderDeps(outputs, references, drvToAttr), "a:b,c;b:c");
    }
    {
        // A dependency cycle must not loop forever.
        std::vector<std::pair<AttrPath, std::string>> outputs = {
            {AttrPath{"a"}, "/nix/store/da.drv"},
            {AttrPath{"b"}, "/nix/store/db.drv"},
        };
        std::map<std::string, AttrPath> drvToAttr = {
            {"/nix/store/da.drv", AttrPath{"a"}},
            {"/nix/store/db.drv", AttrPath{"b"}},
        };
        std::map<std::string, std::vector<std::string>> references = {
            {"/nix/store/da.drv", {"/nix/store/db.drv"}},
            {"/nix/store/db.drv", {"/nix/store/da.drv"}},
        };
        expectEq("cycles are handled",
                 renderDeps(outputs, references, drvToAttr), "a:b;b:a");
    }
    {
        // Two attributes are the same derivation (an alias). Neither must be
        // reported as depending on the other or on itself.
        std::vector<std::pair<AttrPath, std::string>> outputs = {
            {AttrPath{"packages", "default"}, "/nix/store/shared.drv"},
            {AttrPath{"packages", "hello"}, "/nix/store/shared.drv"},
        };
        std::map<std::string, AttrPath> drvToAttr = {
            {"/nix/store/shared.drv", AttrPath{"packages", "default"}},
        };
        std::map<std::string, std::vector<std::string>> references = {
            {"/nix/store/shared.drv", {}},
        };
        expectEq("aliased derivation has no self dependency",
                 renderDeps(outputs, references, drvToAttr), "");
    }

    {
        using namespace std::chrono_literals;

        // A pool with an idle worker, an empty queue, or no headroom left has
        // no reason to grow, however long ago it last grew.
        auto busyBacklog = [] {
            PoolSizing sizing;
            sizing.queued = 10;
            sizing.poolSize = 2;
            sizing.idleWorkers = 0;
            sizing.evaluatedDerivations = 20;
            sizing.maxWorkers = 8;
            sizing.elapsed = 10s;
            sizing.rampInterval = 1s;
            return sizing;
        };

        expectBool("first worker is forked without waiting",
                   shouldSpawnWorker([] {
                       PoolSizing sizing;
                       sizing.poolSize = 0;
                       sizing.maxWorkers = 8;
                       return sizing;
                   }()),
                   true);
        expectBool("a backlog no worker can take grows the pool",
                   shouldSpawnWorker(busyBacklog()), true);
        expectBool("an idle worker takes the work instead of a new one",
                   shouldSpawnWorker([&] {
                       auto sizing = busyBacklog();
                       sizing.idleWorkers = 1;
                       return sizing;
                   }()),
                   false);
        expectBool("an empty queue does not grow the pool",
                   shouldSpawnWorker([&] {
                       auto sizing = busyBacklog();
                       sizing.queued = 0;
                       return sizing;
                   }()),
                   false);
        expectBool("the pool never exceeds its maximum", shouldSpawnWorker([&] {
                       auto sizing = busyBacklog();
                       sizing.poolSize = 8;
                       return sizing;
                   }()),
                   false);
        expectBool("a run shorter than the pool has cost does not grow it",
                   shouldSpawnWorker([&] {
                       auto sizing = busyBacklog();
                       sizing.poolSize = 6;
                       sizing.rampInterval = 2s;
                       return sizing; // 6 workers at 2s each, only 10s elapsed
                   }()),
                   false);
        expectBool("a run that outlasts the pool's cost grows it",
                   shouldSpawnWorker([&] {
                       auto sizing = busyBacklog();
                       sizing.poolSize = 4;
                       sizing.rampInterval = 2s;
                       return sizing; // 4 workers at 2s each, 10s elapsed
                   }()),
                   true);
        expectBool("the pool does not outrun the work seen through",
                   shouldSpawnWorker([&] {
                       auto sizing = busyBacklog();
                       sizing.poolSize = 3;
                       sizing.evaluatedDerivations = 2;
                       return sizing;
                   }()),
                   false);
        expectBool("each completed attribute earns at most one worker",
                   shouldSpawnWorker([&] {
                       auto sizing = busyBacklog();
                       sizing.poolSize = 2;
                       sizing.evaluatedDerivations = 2;
                       return sizing;
                   }()),
                   true);
        expectBool("an untimed run still grows as results come in",
                   shouldSpawnWorker([&] {
                       auto sizing = busyBacklog();
                       sizing.poolSize = 2;
                       sizing.evaluatedDerivations = 3;
                       sizing.rampInterval = 0s; // nothing timed yet
                       return sizing;
                   }()),
                   true);
        expectBool("a rising cost estimate stalls an already-grown pool",
                   shouldSpawnWorker([&] {
                       auto sizing = busyBacklog();
                       sizing.poolSize = 20;
                       sizing.maxWorkers = 64;
                       sizing.rampInterval = 2s;
                       return sizing;
                   }()),
                   false);
        expectBool("a maxed-out pool is not waiting on the ramp",
                   spawnPendingOnRamp([&] {
                       auto sizing = busyBacklog();
                       sizing.poolSize = 8;
                       return sizing;
                   }()),
                   false);
        expectBool("a rate-limited pool is waiting on the ramp",
                   spawnPendingOnRamp(busyBacklog()), true);
        // Walking the attribute tree produces thousands of results before the
        // first derivation, each one a symbol lookup costing almost nothing.
        // If those counted as evidence the pool would reach its ceiling before
        // anything had been evaluated or timed, which is the all-at-once
        // startup the ramp exists to prevent.
        expectBool("a huge backlog with nothing evaluated cannot grow the pool",
                   shouldSpawnWorker([&] {
                       auto sizing = busyBacklog();
                       sizing.queued = 10000;
                       sizing.poolSize = 1;
                       sizing.evaluatedDerivations = 0;
                       return sizing;
                   }()),
                   false);
        expectBool("the first derivation lets the pool grow",
                   shouldSpawnWorker([&] {
                       auto sizing = busyBacklog();
                       sizing.queued = 10000;
                       sizing.poolSize = 1;
                       sizing.evaluatedDerivations = 1;
                       return sizing;
                   }()),
                   true);
        // Only a result can lift the evidence bound, so a caller told it is
        // waiting on the clock would spin instead of waiting for its workers.
        expectBool("a pool short of evidence is not waiting on the ramp",
                   spawnPendingOnRamp([&] {
                       auto sizing = busyBacklog();
                       sizing.poolSize = 3;
                       sizing.evaluatedDerivations = 2;
                       return sizing;
                   }()),
                   false);
        expectBool("the wait is until the run outlasts the pool's cost",
                   timeUntilSpawnAllowed([&] {
                       auto sizing = busyBacklog();
                       sizing.poolSize = 6;
                       sizing.rampInterval = 2s;
                       return sizing;
                   }()) == std::chrono::seconds(2),
                   true);
        expectBool("no wait once growing is already allowed",
                   timeUntilSpawnAllowed(busyBacklog()) ==
                       std::chrono::steady_clock::duration::zero(),
                   true);

        // A worker's startup cost is what its first attribute took beyond an
        // ordinary one, so the ramp slows down as expensive workers are seen.
        WorkerCostModel model;
        expectBool("nothing measured yet leaves the pool free to move",
                   model.rampInterval() == WorkerCostModel::Duration::zero(),
                   true);
        // The first worker has no warm one to compare against, so all of its
        // time to a first derivation counts as what a worker costs.
        model.recordFirstResult(3s);
        expectBool("a first worker's whole delay is its startup cost",
                   model.rampInterval() == std::chrono::seconds(3), true);
        model.recordAttr(1s);
        model.recordAttr(1s);
        model.recordFirstResult(5s);
        expectBool("startup cost is the excess over a warm evaluation",
                   model.rampInterval() == std::chrono::seconds(4), true);
        model.recordFirstResult(1s);
        expectBool("a cheaper worker does not lower the estimate",
                   model.rampInterval() == std::chrono::seconds(4), true);
    }

    {
        std::vector<std::string> required = {"--flake", ".", "--system",
                                             "x86_64-linux"};
        auto withFlags = [&](const std::vector<std::string> & extra) {
            auto arguments = required;
            arguments.insert(arguments.end(), extra.begin(), extra.end());
            return arguments;
        };

        expectBool("dependency edges are reported unless declined",
                   parse(required).dependencies, true);
        expectBool("--no-dependencies declines them",
                   parse(withFlags({"--no-dependencies"})).dependencies, false);
        expectBool("import-from-derivation is allowed unless declined",
                   parse(required).importFromDerivation, true);
        expectBool("--no-import-from-derivation forbids it",
                   parse(withFlags({"--no-import-from-derivation"}))
                       .importFromDerivation,
                   false);
        expectBool("no --workers leaves the ceiling to the machine",
                   parse(required).workers.has_value(), false);
        expectBool("--workers sets the ceiling",
                   parse(withFlags({"--workers", "6"})).workers ==
                       std::optional<std::size_t>(6),
                   true);

        // A zero is a mistake, not a second meaning for either flag.
        // Asking for the usage text is answered, not refused, so that --help
        // can succeed on stdout instead of being reported as a failure.
        expectBool("--help asks for the usage text", asksForHelp({"--help"}),
                   true);
        expectBool("-h asks for the usage text", asksForHelp({"-h"}), true);
        expectBool("--help is not an error", rejects({"--help"}), false);

        expectBool("--workers 0 is rejected",
                   rejects(withFlags({"--workers", "0"})), true);
        expectBool("--max-memory-size 0 is rejected",
                   rejects(withFlags({"--max-memory-size", "0"})), true);
        expectBool("a negative count is rejected",
                   rejects(withFlags({"--workers", "-1"})), true);
        // Leading whitespace used to hide a negation, which then wrapped to
        // about 1.8e19 and left the pool with no ceiling at all.
        expectBool("a negative count behind whitespace is rejected",
                   rejects(withFlags({"--workers", " -1"})), true);
        expectBool("a count behind whitespace is rejected",
                   rejects(withFlags({"--workers", " 4"})), true);
        expectBool("an explicitly signed count is rejected",
                   rejects(withFlags({"--workers", "+4"})), true);
        expectBool("a negative memory ceiling behind whitespace is rejected",
                   rejects(withFlags({"--max-memory-size", " -1"})), true);
        expectBool("a non-numeric count is rejected",
                   rejects(withFlags({"--workers", "six"})), true);
        expectBool("trailing characters are rejected",
                   rejects(withFlags({"--workers", "6x"})), true);
        expectBool("--flake is required", rejects({"--system", "x86_64-linux"}),
                   true);
        expectBool("--system is required", rejects({"--flake", "."}), true);
        expectBool("an unknown flag is rejected",
                   rejects(withFlags({"--log-format", "internal-json"})), true);
    }

    if (failures > 0) {
        fprintf(stderr, "%d assertion(s) failed\n", failures);
        return 1;
    }
    printf("all pure-helper assertions passed\n");
    return 0;
}
