#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "args.hh"

namespace nixcieval {

class Evaluator;

// Serialise a line, replacing anything that is not valid UTF-8 rather than
// refusing to encode it.
//
// Attribute names and Nix error text are whatever a flake author wrote, and
// Nix does not require either to be valid UTF-8. The strict serialiser throws
// on a stray byte, and that exception would travel as a fatal worker error and
// abandon the whole run, so one bad byte in one attribute name would cost
// every result the run had already produced.
std::string dumpLossy(const nlohmann::json & value);

// Run the worker loop in the current (forked) process: evaluate the flake once
// and then serve per-attribute requests read from 'fromParentFd', writing
// results to 'toParentFd'. Never returns normally; it exits the process.
[[noreturn]] void runWorker(const Args & args, int toParentFd,
                            int fromParentFd);

// Run the same loop on an evaluator inherited from the process that forked
// this one, which has already loaded the flake and forced what its outputs
// share. Takes over the store first, since that is the one thing a fork must
// not leave shared. Never returns normally; it exits the process.
[[noreturn]] void runInheritedWorker(const Args & args, Evaluator & evaluator,
                                     int toParentFd, int fromParentFd);

} // namespace nixcieval
