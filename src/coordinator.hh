#pragma once

#include "args.hh"

namespace nixcieval {

// Fork the evaluation workers, drive discovery to completion streaming one
// JSON object per output to stdout, and (when requested) emit dependency edges
// at the end. Returns a process exit code.
int runCoordinator(const Args & args);

} // namespace nixcieval
