#pragma once

#include <chrono>

namespace nixcieval {

// Timing traces, on stderr, for finding out where a run's time goes: what a
// worker costs to start and what an attribute costs once it is running are
// what decide whether a pool is worth having.
//
// Off unless NIX_CI_EVAL_TRACE is set in the environment. The output is not
// part of this tool's contract, which is the JSON on stdout; it is for
// measuring changes to the tool itself and is free to change with them.
bool traceEnabled();

// A duration in seconds, for a trace line to report.
double traceSeconds(std::chrono::steady_clock::duration duration);

// Seconds since 'since', likewise.
double traceSecondsSince(std::chrono::steady_clock::time_point since);

} // namespace nixcieval
