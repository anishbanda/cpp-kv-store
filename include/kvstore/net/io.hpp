#pragma once

#include <cstddef>

#include "kvstore/net/bounded_buffer.hpp"

namespace kvstore::net {

enum class ReadStatus {
  kReadSome,    // read at least one byte into `buffer`
  kWouldBlock,  // no data available right now (EAGAIN/EWOULDBLOCK) or interrupted (EINTR)
  kClosed,      // peer performed an orderly shutdown (recv returned 0)
  kBufferFull,  // `buffer` has no remaining capacity; nothing was read
  kError,       // an unexpected errno; see `error_number`
};

struct ReadOutcome {
  ReadStatus status;
  std::size_t bytes_read = 0;
  int error_number = 0;  // valid iff status == kError
};

// Performs at most one nonblocking read from `fd` into the room remaining
// under `buffer.capacity()`. Never blocks. Intended to be called from an
// epoll-driven event loop (Milestone 4) once `fd` is reported readable.
[[nodiscard]] ReadOutcome read_into(int fd, BoundedBuffer& buffer);

enum class WriteStatus {
  kWroteSome,   // wrote (and consumed) some or all pending bytes, possibly zero if `buffer` was
                // empty
  kWouldBlock,  // the socket send buffer is full (EAGAIN/EWOULDBLOCK) or interrupted (EINTR)
  kClosed,      // the peer has hung up (EPIPE/ECONNRESET); no SIGPIPE is raised (see below)
  kError,       // an unexpected errno; see `error_number`
};

struct WriteOutcome {
  WriteStatus status;
  std::size_t bytes_written = 0;
  int error_number = 0;  // valid iff status == kError
};

// Performs at most one nonblocking write of `buffer.view()` to `fd`, then
// consumes exactly the bytes actually written. Never blocks. Uses
// MSG_NOSIGNAL so a peer that has closed its read side cannot raise
// SIGPIPE and terminate the process (SPEC.md section 5).
[[nodiscard]] WriteOutcome write_from(int fd, BoundedBuffer& buffer);

}  // namespace kvstore::net
