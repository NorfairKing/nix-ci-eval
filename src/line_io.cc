#include "line_io.hh"

#include <cerrno>
#include <cstddef>
#include <unistd.h>
#include <utility>

namespace nixcieval {

LineReader::LineReader(int fd) : fd_(fd) {}

LineReader::~LineReader() { close(); }

LineReader::LineReader(LineReader && other) noexcept
    : fd_(other.fd_), buffer_(std::move(other.buffer_)) {
    other.fd_ = -1;
}

LineReader & LineReader::operator=(LineReader && other) noexcept {
    if (this != &other) {
        close();
        fd_ = other.fd_;
        buffer_ = std::move(other.buffer_);
        other.fd_ = -1;
    }
    return *this;
}

void LineReader::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

std::string LineReader::readLine() {
    while (true) {
        auto newline = buffer_.find('\n');
        if (newline != std::string::npos) {
            std::string line = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            return line;
        }

        char chunk[4096];
        ssize_t got = ::read(fd_, chunk, sizeof(chunk));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            // Treat a read error like end of file: return whatever is buffered.
            std::string line = std::move(buffer_);
            buffer_.clear();
            return line;
        }
        if (got == 0) {
            // End of file. Return any trailing unterminated data once, then
            // empty strings forever.
            std::string line = std::move(buffer_);
            buffer_.clear();
            return line;
        }
        buffer_.append(chunk, static_cast<std::size_t>(got));
        if (buffer_.size() > kMaxLineBytes) {
            // A peer that never sends a newline, or sends a line far larger
            // than any real message, must not be able to grow this buffer
            // until the process dies. Drop what was accumulated so the failure
            // costs the line rather than the machine.
            buffer_.clear();
            throw LineTooLong{};
        }
    }
}

bool LineReader::hasBufferedLine() const {
    return buffer_.find('\n') != std::string::npos;
}

namespace {

// Write all of a buffer, retrying on partial writes and EINTR.
bool writeAll(int fd, const char * data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        ssize_t got = ::write(fd, data + written, size - written);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += static_cast<std::size_t>(got);
    }
    return true;
}

} // namespace

bool writeLine(int fd, const std::string & line) {
    // The pipe has a single writer, and readers reassemble by newline, so
    // writing the body and the terminator separately avoids copying the
    // (potentially large) line just to append one byte.
    return writeAll(fd, line.data(), line.size()) && writeAll(fd, "\n", 1);
}

} // namespace nixcieval
