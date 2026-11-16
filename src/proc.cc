#include "proc.hh"

#include <cstdio>
#include <unistd.h>

#include "pure.hh"

namespace nixcieval {

std::string readProcFile(const char * path) {
    std::FILE * file = std::fopen(path, "r");
    if (file == nullptr) {
        return {};
    }
    char buffer[4096] = {};
    std::size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    return std::string(buffer, read);
}

std::size_t currentPrivateMiB() {
    auto priv =
        privateMiBFromSmapsRollup(readProcFile("/proc/self/smaps_rollup"));
    if (priv) {
        return *priv;
    }
    // Without smaps_rollup there is no split to make, and resident size is the
    // honest answer for a process that shares nothing.
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) {
        return 0;
    }
    return residentMiBFromStatm(readProcFile("/proc/self/statm"),
                                static_cast<std::size_t>(pageSize));
}

std::optional<std::size_t> currentThreadCount() {
    return threadsFromProcStatus(readProcFile("/proc/self/status"));
}

} // namespace nixcieval
