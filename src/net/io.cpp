#include "kvstore/net/io.hpp"

#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <string_view>

namespace kvstore::net {

namespace {

constexpr std::size_t kReadChunkSize = 64 * 1024;

}  // namespace

ReadOutcome read_into(int fd, BoundedBuffer& buffer) {
  const std::size_t available = buffer.capacity() - buffer.size();
  if (available == 0) {
    return ReadOutcome{.status = ReadStatus::kBufferFull, .bytes_read = 0, .error_number = 0};
  }

  // A reused stack buffer, not a freshly allocated std::string: recv()
  // overwrites exactly the bytes it returns, so there is nothing to
  // zero-initialize first. Milestone 9 profiling found the previous
  // std::string(n, '\0') version spending over half of all instructions
  // in memset, zero-filling up to 64 KiB per call only to then discard
  // whatever recv() didn't overwrite.
  static thread_local char chunk[kReadChunkSize];
  const std::size_t attempt = std::min(available, kReadChunkSize);
  const ssize_t n = ::recv(fd, chunk, attempt, 0);

  if (n > 0) {
    // `attempt` already bounded `n`, so this always succeeds.
    const bool appended = buffer.append(std::string_view(chunk, static_cast<std::size_t>(n)));
    (void)appended;
    return ReadOutcome{.status = ReadStatus::kReadSome,
                       .bytes_read = static_cast<std::size_t>(n),
                       .error_number = 0};
  }
  if (n == 0) {
    return ReadOutcome{.status = ReadStatus::kClosed, .bytes_read = 0, .error_number = 0};
  }

  if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
    return ReadOutcome{.status = ReadStatus::kWouldBlock, .bytes_read = 0, .error_number = 0};
  }
  return ReadOutcome{.status = ReadStatus::kError, .bytes_read = 0, .error_number = errno};
}

WriteOutcome write_from(int fd, BoundedBuffer& buffer) {
  if (buffer.empty()) {
    return WriteOutcome{.status = WriteStatus::kWroteSome, .bytes_written = 0, .error_number = 0};
  }

  const std::string_view view = buffer.view();
  const ssize_t n = ::send(fd, view.data(), view.size(), MSG_NOSIGNAL);

  if (n > 0) {
    buffer.consume(static_cast<std::size_t>(n));
    return WriteOutcome{.status = WriteStatus::kWroteSome,
                        .bytes_written = static_cast<std::size_t>(n),
                        .error_number = 0};
  }
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return WriteOutcome{
          .status = WriteStatus::kWouldBlock, .bytes_written = 0, .error_number = 0};
    }
    if (errno == EPIPE || errno == ECONNRESET) {
      return WriteOutcome{
          .status = WriteStatus::kClosed, .bytes_written = 0, .error_number = errno};
    }
    return WriteOutcome{.status = WriteStatus::kError, .bytes_written = 0, .error_number = errno};
  }
  // send() returning exactly 0 for a nonzero-length request is not a
  // defined outcome for stream sockets; treat it as "nothing written yet".
  return WriteOutcome{.status = WriteStatus::kWouldBlock, .bytes_written = 0, .error_number = 0};
}

}  // namespace kvstore::net
