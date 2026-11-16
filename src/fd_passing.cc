#include "fd_passing.hh"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace nixcieval {

namespace {

// The most one control message may carry. Two descriptors (a worker's two
// pipe ends) is all this protocol sends; the limit is what a malformed peer
// is held to.
constexpr std::size_t kMaxFdsPerMessage = 8;

// Enough for any request this protocol sends, which are a few bytes of JSON.
// A larger message is truncated by the kernel rather than split, and the
// caller sees an unparseable request, which is how it treats any other
// garbage from a peer.
constexpr std::size_t kMaxMessageBytes = 4096;

} // namespace

bool sendWithFds(int socket, const std::string & message,
                 const std::vector<int> & fds) {
    if (fds.size() > kMaxFdsPerMessage) {
        return false;
    }

    iovec body{};
    // sendmsg does not write through this pointer, but its type is not const.
    body.iov_base = const_cast<char *>(message.data());
    body.iov_len = message.size();

    // The control buffer must be aligned for cmsghdr, which is what the union
    // with a cmsghdr provides.
    union {
        char buffer[CMSG_SPACE(sizeof(int) * kMaxFdsPerMessage)];
        cmsghdr align;
    } control{};

    msghdr header{};
    header.msg_iov = &body;
    header.msg_iovlen = 1;
    if (!fds.empty()) {
        header.msg_control = control.buffer;
        header.msg_controllen = CMSG_SPACE(sizeof(int) * fds.size());
        cmsghdr * cmsg = CMSG_FIRSTHDR(&header);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
        std::memcpy(CMSG_DATA(cmsg), fds.data(), sizeof(int) * fds.size());
    }

    while (true) {
        ssize_t sent = sendmsg(socket, &header, 0);
        if (sent >= 0) {
            return static_cast<std::size_t>(sent) == message.size();
        }
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
}

FdMessage receiveWithFds(int socket, std::size_t maxFds) {
    char body[kMaxMessageBytes];
    iovec target{};
    target.iov_base = body;
    target.iov_len = sizeof(body);

    union {
        char buffer[CMSG_SPACE(sizeof(int) * kMaxFdsPerMessage)];
        cmsghdr align;
    } control{};

    msghdr header{};
    header.msg_iov = &target;
    header.msg_iovlen = 1;
    header.msg_control = control.buffer;
    header.msg_controllen = sizeof(control.buffer);

    ssize_t got = 0;
    while (true) {
        got = recvmsg(socket, &header, 0);
        if (got >= 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        throw std::runtime_error(std::string("recvmsg failed: ") +
                                 std::strerror(errno));
    }

    FdMessage received;
    if (got == 0) {
        received.closed = true;
        return received;
    }
    received.message.assign(body, static_cast<std::size_t>(got));

    for (cmsghdr * cmsg = CMSG_FIRSTHDR(&header); cmsg != nullptr;
         cmsg = CMSG_NXTHDR(&header, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) {
            continue;
        }
        std::size_t count = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (std::size_t i = 0; i < count; i++) {
            int fd = 0;
            std::memcpy(&fd, CMSG_DATA(cmsg) + i * sizeof(int), sizeof(int));
            // A descriptor beyond what the caller asked for is closed here
            // rather than returned: it is already in this process's table, so
            // ignoring it would leak it for the life of the process.
            if (received.fds.size() >= maxFds) {
                ::close(fd);
                continue;
            }
            received.fds.push_back(fd);
        }
    }

    return received;
}

} // namespace nixcieval
