#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "kvstore/net/bounded_buffer.hpp"
#include "kvstore/net/connection_id.hpp"
#include "kvstore/net/file_descriptor.hpp"
#include "kvstore/net/response_sequencer.hpp"

namespace kvstore::net {

// Default per-connection buffer caps (SPEC.md section 5, Networking
// Requirements: "Cap each connection's unread input buffer at 2 MiB").
// ARCHITECTURE.md section 4 leaves the output watermark to be chosen and
// documented here: it mirrors the input cap so a slow reader can build up
// at most as much unsent data as a slow writer could send unread.
inline constexpr std::size_t kDefaultMaxInputBufferBytes = 2 * 1024 * 1024;
inline constexpr std::size_t kDefaultMaxOutputBufferBytes = 2 * 1024 * 1024;

// One accepted client connection: its socket plus bounded input/output
// buffers. Exclusively owned and used by the event-loop thread (see
// ARCHITECTURE.md, Data Ownership, and docs/CONCURRENCY.md); worker
// threads never touch a Connection directly, only its (id, generation).
class Connection {
 public:
  Connection(ConnectionId id, ConnectionGeneration generation, FileDescriptor fd,
             std::string peer_address, std::size_t max_input_bytes = kDefaultMaxInputBufferBytes,
             std::size_t max_output_bytes = kDefaultMaxOutputBufferBytes);

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;
  Connection(Connection&&) = default;
  Connection& operator=(Connection&&) = default;

  [[nodiscard]] ConnectionId id() const noexcept { return id_; }
  [[nodiscard]] ConnectionGeneration generation() const noexcept { return generation_; }
  [[nodiscard]] int fd() const noexcept { return fd_.get(); }
  [[nodiscard]] const std::string& peer_address() const noexcept { return peer_address_; }

  [[nodiscard]] BoundedBuffer& input() noexcept { return input_; }
  [[nodiscard]] const BoundedBuffer& input() const noexcept { return input_; }
  [[nodiscard]] BoundedBuffer& output() noexcept { return output_; }
  [[nodiscard]] const BoundedBuffer& output() const noexcept { return output_; }

  [[nodiscard]] ResponseSequencer& sequencer() noexcept { return sequencer_; }

 private:
  ConnectionId id_;
  ConnectionGeneration generation_;
  FileDescriptor fd_;
  std::string peer_address_;
  BoundedBuffer input_;
  BoundedBuffer output_;
  ResponseSequencer sequencer_;
};

}  // namespace kvstore::net
