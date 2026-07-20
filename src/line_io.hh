#pragma once

#include <cstddef>
#include <string>

namespace nixcieval {

// The most a single message may occupy before the reader gives up on it. Far
// above any real attribute path or error message, and small enough that a peer
// cannot spend the reader's memory.
constexpr std::size_t kMaxLineBytes = 8 * 1024 * 1024;

// Thrown by readLine when a message exceeds kMaxLineBytes. Carries nothing: to
// the caller this is the same class of problem as a peer that produced garbage.
struct LineTooLong {};

// A buffered reader of newline-delimited messages from a file descriptor.
// Owns the descriptor and closes it on destruction.
class LineReader {
  public:
    explicit LineReader(int fd);
    ~LineReader();

    LineReader(const LineReader &) = delete;
    LineReader & operator=(const LineReader &) = delete;
    LineReader(LineReader && other) noexcept;
    LineReader & operator=(LineReader && other) noexcept;

    // Read one line (without the trailing newline). Returns an empty string at
    // end of file, which callers treat as "peer closed the pipe".
    std::string readLine();

    // Whether a complete line is already buffered and can be returned by
    // readLine() without touching the file descriptor. Used to drain data that
    // arrived together with a poll-triggering read but would not re-trigger
    // poll on its own.
    bool hasBufferedLine() const;

    int fd() const { return fd_; }

  private:
    void close();

    int fd_;
    std::string buffer_;
};

// Write a message followed by a newline to a descriptor. Returns false if the
// write failed (e.g. the peer closed the pipe).
bool writeLine(int fd, const std::string & line);

} // namespace nixcieval
