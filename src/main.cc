#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>

#include "args.hh"
#include "coordinator.hh"

int main(int argc, char ** argv) {
    // Garbage collection is handled by discarding forked workers, so disable
    // the Boehm GC (must happen before initGC).
    setenv("GC_DONT_GC", "1", 1);
    // A worker closing its pipe should surface as a write error, not a signal.
    signal(SIGPIPE, SIG_IGN);

    try {
        nixcieval::Args args = nixcieval::parseArgs(argc, argv);
        return nixcieval::runCoordinator(args);
    } catch (const nixcieval::HelpRequested & help) {
        fputs(help.usage.c_str(), stdout);
        return 0;
    } catch (const std::exception & e) {
        fprintf(stderr, "nix-ci-eval: %s\n", e.what());
        return 1;
    }
}
