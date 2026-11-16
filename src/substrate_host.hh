#pragma once

#include "args.hh"

namespace nixcieval {

// Run the substrate host in the current (forked) process: load the flake,
// force what its outputs share, and then fork one evaluation worker per
// request read from 'controlFd', which carries that worker's pipe endpoints
// with it.
//
// Workers forked here inherit the forced evaluator, so the flake's shared
// substrate is evaluated once for the run rather than once per worker. The
// host evaluates nothing else afterwards, which is what keeps it able to
// answer a fork request immediately and keeps its memory flat, so that the
// pages its workers share stay shared.
//
// Never returns; it exits the process.
[[noreturn]] void runSubstrateHost(const Args & args, int controlFd);

} // namespace nixcieval
