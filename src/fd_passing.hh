#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace nixcieval {

// Passing file descriptors between processes, so that one process can be made
// to hold a pipe that another process created.
//
// The coordinator creates a worker's pipes itself and hands the worker's ends
// to the process that forks it. Keeping pipe creation where the parent-side
// ends are kept is what lets a worker forked elsewhere be spoken to exactly
// like one the coordinator forked.
//
// The socket must be a SOCK_SEQPACKET pair: one send is one receive, so a
// message and the descriptors attached to it never arrive split or joined,
// which a stream socket would allow.

// Send one message, with 'fds' attached. Returns false if it could not be
// sent, including when the peer is gone.
bool sendWithFds(int socket, const std::string & message,
                 const std::vector<int> & fds);

// One received message and whatever descriptors came with it.
struct FdMessage {
    std::string message;
    std::vector<int> fds;
    // Set when the peer closed the socket, which is the end of the
    // conversation rather than a message.
    bool closed = false;
};

// Receive one message and up to 'maxFds' descriptors. Descriptors beyond that
// are closed rather than returned, so a peer cannot fill this process's
// descriptor table. Throws std::runtime_error if the socket cannot be read.
FdMessage receiveWithFds(int socket, std::size_t maxFds);

} // namespace nixcieval
