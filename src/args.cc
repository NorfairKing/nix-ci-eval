#include "args.hh"

#include <charconv>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace nixcieval {

namespace {

const char * const USAGE =
    "nix-ci-eval --flake <ref> --system <system> [options]\n"
    "\n"
    "Discover the buildable outputs of a flake for one system and stream one\n"
    "JSON object per output (packages, checks, devShells, formatter, "
    "devShell).\n"
    "\n"
    "Options:\n"
    "  --flake <ref>                Flake reference to discover (required)\n"
    "  --system <system>            Target system, e.g. x86_64-linux "
    "(required)\n"
    "  --workers <n>                Maximum parallel evaluator processes; the "
    "run\n"
    "                               uses as many as the work justifies "
    "(default: CPU count)\n"
    "  --max-memory-size <MiB>      Per-worker memory ceiling (default: 4096)\n"
    "  --no-dependencies            Skip the dependency edges between outputs\n"
    "  --no-import-from-derivation  Disallow import-from-derivation\n"
    "  --impure                     Evaluate impurely (default: pure)\n"
    "  --no-shallow                 Fetch a remote git flake's whole history, "
    "which\n"
    "                               a flake reading self.revCount needs\n"
    "  --help, -h                   Show this help\n";

// Parse a count that must be at least one. Zero is rejected rather than given
// a second meaning: neither a pool of no workers nor a memory ceiling nothing
// can stay under is a thing to ask for, so a zero is a mistake worth naming.
std::size_t parsePositive(std::string_view flag, const char * value) {
    std::string text(value);
    auto invalid = [&]() {
        return std::runtime_error("invalid number for " + std::string(flag) +
                                  ": " + value);
    };
    // Digits and nothing else. Anything more permissive has to contend with
    // std::stoul, which skips leading whitespace and turns a negation into a
    // huge positive value, so " -1" would arrive here as a worker count of
    // about 1.8e19 and remove the ceiling on the pool entirely.
    if (text.empty()) {
        throw invalid();
    }
    for (char c : text) {
        if (c < '0' || c > '9') {
            throw invalid();
        }
    }
    std::size_t parsed = 0;
    auto [end, ec] =
        std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (ec != std::errc() || end != text.data() + text.size()) {
        throw invalid();
    }
    if (parsed == 0) {
        throw std::runtime_error(std::string(flag) + " must be at least 1");
    }
    return parsed;
}

} // namespace

Args parseArgs(int argc, char ** argv) {
    Args args;
    bool haveFlake = false;
    bool haveSystem = false;

    for (int i = 1; i < argc; i++) {
        std::string_view arg = argv[i];
        auto next = [&](std::string_view flag) -> const char * {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " +
                                         std::string(flag) + "\n\n" + USAGE);
            }
            return argv[++i];
        };

        if (arg == "--flake") {
            args.flakeRef = next(arg);
            haveFlake = true;
        } else if (arg == "--system") {
            args.system = next(arg);
            haveSystem = true;
        } else if (arg == "--workers") {
            args.workers = parsePositive(arg, next(arg));
        } else if (arg == "--max-memory-size") {
            args.maxMemoryMiB = parsePositive(arg, next(arg));
        } else if (arg == "--no-dependencies") {
            args.dependencies = false;
        } else if (arg == "--no-import-from-derivation") {
            args.importFromDerivation = false;
        } else if (arg == "--impure") {
            args.impure = true;
        } else if (arg == "--no-shallow") {
            args.shallow = false;
        } else if (arg == "--help" || arg == "-h") {
            throw HelpRequested{USAGE};
        } else {
            throw std::runtime_error("unknown argument: " + std::string(arg) +
                                     "\n\n" + USAGE);
        }
    }

    if (!haveFlake) {
        throw std::runtime_error("missing required --flake\n\n" +
                                 std::string(USAGE));
    }
    if (!haveSystem) {
        throw std::runtime_error("missing required --system\n\n" +
                                 std::string(USAGE));
    }

    return args;
}

} // namespace nixcieval
