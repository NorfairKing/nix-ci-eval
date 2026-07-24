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
        // Drive an EdgeDiscovery to exhaustion against a fixed graph, adding
        // the outputs in the given order, and render the edges in the order
        // they were reported as "from->to,from->to".
        auto runDiscovery =
            [](const std::vector<std::pair<AttrPath, std::string>> & outputs,
               const std::map<std::string, std::vector<std::string>> & graph) {
                EdgeDiscovery discovery;
                std::string out;
                auto record = [&](const std::vector<OutputEdge> & edges) {
                    for (const auto & edge : edges) {
                        if (!out.empty()) {
                            out += ",";
                        }
                        out +=
                            renderPath(edge.from) + "->" + renderPath(edge.to);
                    }
                };
                for (const auto & [attr, drvPath] : outputs) {
                    record(discovery.addOutput(attr, drvPath));
                }
                while (auto query = discovery.nextQuery()) {
                    auto found = graph.find(*query);
                    record(discovery.provideReferences(
                        *query, found == graph.end()
                                    ? std::vector<std::string>{}
                                    : found->second));
                }
                return out;
            };

        std::map<std::string, std::vector<std::string>> graph = {
            {"/a.drv", {"/b.drv", "/lib.drv"}},
            {"/b.drv", {"/c.drv"}},
            {"/c.drv", {}},
            {"/lib.drv", {}},
        };

        // a depends on b directly and c through it; b depends on c. The edges
        // are the same however the outputs happen to be discovered, which is
        // what lets each one be reported the moment it is known.
        expectEq("edges found walking forwards",
                 runDiscovery({{AttrPath{"a"}, "/a.drv"},
                               {AttrPath{"b"}, "/b.drv"},
                               {AttrPath{"c"}, "/c.drv"}},
                              graph),
                 "a->b,a->c,b->c");
        expectEq("the same edges when the outputs arrive in reverse",
                 runDiscovery({{AttrPath{"c"}, "/c.drv"},
                               {AttrPath{"b"}, "/b.drv"},
                               {AttrPath{"a"}, "/a.drv"}},
                              graph),
                 "b->c,a->b,a->c");

        // An output discovered after the walks have already passed through its
        // derivation still gets its edges: that is the case that would be lost
        // by only looking forwards.
        expectEq("an output discovered late is still joined up",
                 runDiscovery(
                     {{AttrPath{"a"}, "/a.drv"}, {AttrPath{"lib"}, "/lib.drv"}},
                     graph),
                 "a->lib");

        // A cycle must not loop forever, and neither end depends on itself.
        std::map<std::string, std::vector<std::string>> cyclic = {
            {"/x.drv", {"/y.drv"}},
            {"/y.drv", {"/x.drv"}},
        };
        expectEq(
            "a cycle terminates",
            runDiscovery({{AttrPath{"x"}, "/x.drv"}, {AttrPath{"y"}, "/y.drv"}},
                         cyclic),
            "x->y,y->x");

        // Two attributes that are the same derivation: the second is not a
        // separate node, so neither stands in its own closure.
        expectEq("an aliased derivation reports no edge",
                 runDiscovery(
                     {{AttrPath{"one"}, "/c.drv"}, {AttrPath{"two"}, "/c.drv"}},
                     graph),
                 "");

        // Nothing to depend on at all.
        expectEq("an output with no discovered dependencies reports none",
                 runDiscovery({{AttrPath{"solo"}, "/lib.drv"}}, graph), "");
    }

    {
        // The point of the whole exercise: an edge is reported before the
        // walk that found it has finished, so a consumer hears about it as
        // soon as it is true rather than once everything is known.
        EdgeDiscovery discovery;
        std::map<std::string, std::vector<std::string>> deep = {
            {"/top.drv", {"/dep.drv"}},
            {"/dep.drv", {"/n1.drv"}},
            {"/n1.drv", {"/n2.drv"}},
            {"/n2.drv", {"/n3.drv"}},
            {"/n3.drv", {}},
        };
        discovery.addOutput(AttrPath{"top"}, "/top.drv");
        discovery.addOutput(AttrPath{"dep"}, "/dep.drv");

        // Answer only the first path. That is enough to know top->dep, while
        // most of top's closure is still unwalked.
        auto query = discovery.nextQuery();
        expectBool("the first query is asked", query.has_value(), true);
        auto edges = discovery.provideReferences(*query, deep.at(*query));
        expectBool("the edge is known before the walk finishes",
                   edges.size() == 1, true);
        expectBool("and the walk is indeed unfinished", discovery.done(),
                   false);
    }

    {
        // An output can be discovered after another output's walk has already
        // passed through its derivation and run dry. The walk that starts for
        // it then begins on a path whose references are already known, so no
        // query can move it along, and only draining it on the spot finishes
        // it. Leave it stuck and the caller is left with a discovery that is
        // never done and never has anything to ask, which is a spin rather
        // than a wait.
        EdgeDiscovery discovery;
        discovery.addOutput(AttrPath{"top"}, "/top.drv");
        while (auto query = discovery.nextQuery()) {
            discovery.provideReferences(
                *query, *query == "/top.drv"
                            ? std::vector<std::string>{"/dep.drv"}
                            : std::vector<std::string>{});
        }
        expectBool("the first walk runs dry", discovery.done(), true);

        auto edges = discovery.addOutput(AttrPath{"dep"}, "/dep.drv");
        expectEq("the edge to the late output is reported",
                 edges.size() == 1 ? renderPath(edges[0].from) + "->" +
                                         renderPath(edges[0].to)
                                   : "",
                 "top->dep");
        expectBool("and its own walk needs nothing to finish",
                   discovery.nextQuery().has_value(), false);
        expectBool("so the discovery is done", discovery.done(), true);
    }

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
        expectBool("a remote git ref is fetched shallow unless declined",
                   parse(required).shallow, true);
        expectBool("--no-shallow asks for the whole history",
                   parse(withFlags({"--no-shallow"})).shallow, false);
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
