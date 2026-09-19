#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace nixcieval {

// What a process can find out about itself from /proc. The parsing lives in
// pure.hh, where it is tested without needing a /proc to read.

// The first bytes of a /proc file, or an empty string if it cannot be read.
// These files are generated on read, so they have no size to stat and are
// read into a fixed buffer.
std::string readProcFile(const char * path);

// How much this process is holding that it does not share with the process it
// was forked from, in MiB. Falls back to resident size where the kernel does
// not report the split.
//
// Private rather than total, because a worker that inherited its evaluator
// shares all of it: counting those pages against every worker holding them
// would put the pool over any ceiling the evaluator itself approaches, and
// recycling a worker would not win them back.
std::size_t currentPrivateMiB();

// How many threads this process has, or nothing if that cannot be read.
std::optional<std::size_t> currentThreadCount();

} // namespace nixcieval
