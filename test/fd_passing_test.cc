// Unit tests for passing file descriptors between processes. Exits non-zero
// on the first failed assertion so `meson test` reports it.

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "fd_passing.hh"

namespace {

int failures = 0;

void expect(const char * what, bool condition) {
    if (!condition) {
        failures++;
        fprintf(stderr, "FAIL %s\n", what);
    }
}

// A connected SOCK_SEQPACKET pair, which is what the protocol requires.
bool makeSocketPair(int pair[2]) {
    return socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) == 0;
}

void messageWithoutDescriptorsRoundTrips() {
    int pair[2];
    if (!makeSocketPair(pair)) {
        expect("socketpair", false);
        return;
    }
    expect("send succeeds",
           nixcieval::sendWithFds(pair[0], "{\"fork\":1}", {}));
    auto received = nixcieval::receiveWithFds(pair[1], 2);
    expect("message arrives whole", received.message == "{\"fork\":1}");
    expect("no descriptors arrive", received.fds.empty());
    expect("socket is open", !received.closed);
    close(pair[0]);
    close(pair[1]);
}

// The point of the whole helper: a descriptor created in one process can be
// used by another, which is what lets a worker's pipes be created where its
// parent-side ends are kept and the worker be forked somewhere else.
void passedDescriptorCarriesData() {
    int pair[2];
    if (!makeSocketPair(pair)) {
        expect("socketpair", false);
        return;
    }
    int hose[2];
    if (pipe(hose) != 0) {
        expect("pipe", false);
        return;
    }

    pid_t child = fork();
    if (child == 0) {
        close(pair[0]);
        close(hose[0]);
        close(hose[1]);
        auto received = nixcieval::receiveWithFds(pair[1], 2);
        if (received.fds.size() != 1) {
            _exit(2);
        }
        const char * payload = "through the passed end";
        ssize_t written = write(received.fds[0], payload, std::strlen(payload));
        _exit(written == static_cast<ssize_t>(std::strlen(payload)) ? 0 : 3);
    }

    close(pair[1]);
    expect("send with a descriptor succeeds",
           nixcieval::sendWithFds(pair[0], "take this", {hose[1]}));
    close(hose[1]);

    char buffer[64] = {};
    ssize_t got = read(hose[0], buffer, sizeof(buffer) - 1);
    expect("the child wrote through the descriptor it was passed",
           got > 0 && std::string(buffer) == "through the passed end");

    int status = 0;
    waitpid(child, &status, 0);
    expect("the child exited cleanly",
           WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(hose[0]);
    close(pair[0]);
}

void closedPeerIsReportedAsClosed() {
    int pair[2];
    if (!makeSocketPair(pair)) {
        expect("socketpair", false);
        return;
    }
    close(pair[0]);
    auto received = nixcieval::receiveWithFds(pair[1], 2);
    expect("a closed peer is reported", received.closed);
    expect("a closed peer carries no message", received.message.empty());
    close(pair[1]);
}

// A peer that attaches more descriptors than were asked for must not be able
// to fill this process's descriptor table.
void extraDescriptorsAreNotReturned() {
    int pair[2];
    if (!makeSocketPair(pair)) {
        expect("socketpair", false);
        return;
    }
    int hose[2];
    if (pipe(hose) != 0) {
        expect("pipe", false);
        return;
    }
    expect("send with two descriptors succeeds",
           nixcieval::sendWithFds(pair[0], "too many", {hose[0], hose[1]}));
    auto received = nixcieval::receiveWithFds(pair[1], 1);
    expect("only the asked-for descriptors are returned",
           received.fds.size() == 1);
    for (int fd : received.fds) {
        close(fd);
    }
    close(hose[0]);
    close(hose[1]);
    close(pair[0]);
    close(pair[1]);
}

} // namespace

int main() {
    messageWithoutDescriptorsRoundTrips();
    passedDescriptorCarriesData();
    closedPeerIsReportedAsClosed();
    extraDescriptorsAreNotReturned();

    if (failures > 0) {
        fprintf(stderr, "%d assertion(s) failed\n", failures);
        return 1;
    }
    printf("fd passing tests passed\n");
    return 0;
}
