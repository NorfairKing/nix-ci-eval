#include "trace.hh"

#include <cstdlib>

namespace nixcieval {

bool traceEnabled() {
    // Read once: this is asked per attribute, and a run cannot change its own
    // environment halfway through.
    static const bool enabled = std::getenv("NIX_CI_EVAL_TRACE") != nullptr;
    return enabled;
}

double traceSeconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double>(duration).count();
}

double traceSecondsSince(std::chrono::steady_clock::time_point since) {
    return traceSeconds(std::chrono::steady_clock::now() - since);
}

} // namespace nixcieval
