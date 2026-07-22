#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace nixcieval {

// Thrown when the usage text was asked for rather than needed to explain a
// mistake, so that it can be answered on stdout with a success status instead
// of being reported as an error.
struct HelpRequested {
    std::string usage;
};

// Command-line configuration for a discovery run.
struct Args {
    // The flake reference to discover outputs in (e.g. github:owner/repo).
    std::string flakeRef;
    // The system to discover outputs for (e.g. x86_64-linux).
    std::string system;
    // Ceiling on the number of parallel evaluation worker processes. How many
    // are actually forked is decided during the run from the measured cost of
    // a worker and the size of the backlog. Absent means "as many as the
    // machine has cores".
    std::optional<std::size_t> workers;
    // Per-worker resident-memory ceiling in MiB; a worker that exceeds it is
    // restarted between jobs.
    std::size_t maxMemoryMiB = 4096;
    // After discovery, emit the dependency edges between discovered outputs.
    bool dependencies = true;
    // Allow import-from-derivation during evaluation. On by default so that an
    // output that only resolves through IFD is still discovered; turned off
    // with --no-import-from-derivation.
    bool importFromDerivation = true;
    // Evaluate impurely (allows access to the ambient environment). Flakes are
    // evaluated purely by default.
    bool impure = false;
    // Fetch only the commit being evaluated when the flake reference names a
    // remote git repository, rather than everything it descends from. Turned
    // off with --no-shallow for a flake that reads self.revCount, which Nix
    // cannot supply without the history.
    bool shallow = true;
};

// Parse argv into 'Args'. Throws std::runtime_error with a usage message on a
// missing required argument or an unknown flag.
Args parseArgs(int argc, char ** argv);

} // namespace nixcieval
