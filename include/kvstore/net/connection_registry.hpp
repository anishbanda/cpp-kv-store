#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

#include "kvstore/net/connection.hpp"
#include "kvstore/net/file_descriptor.hpp"

namespace kvstore::net {

// Owns every currently open Connection and hands out the monotonically
// increasing generation that, paired with a connection's id (its fd
// number), lets later code detect whether "connection id X" still refers
// to the same client -- fd numbers are reused by the OS, so id alone is
// not enough (see ARCHITECTURE.md, Data Ownership). Exclusively owned and
// used by the event-loop thread; does no locking of its own.
class ConnectionRegistry {
 public:
  ConnectionRegistry() = default;

  // Registers a newly accepted client as a new connection and returns a
  // reference to it, valid until it is closed.
  Connection& add(FileDescriptor fd, std::string peer_address,
                  std::size_t max_input_bytes = kDefaultMaxInputBufferBytes,
                  std::size_t max_output_bytes = kDefaultMaxOutputBufferBytes);

  // Closes and removes the connection at `id` if it is still open and its
  // generation matches (guards against acting on a stale/reused id).
  // Returns true if a connection was removed.
  bool close(ConnectionId id, ConnectionGeneration generation);

  [[nodiscard]] Connection* find(ConnectionId id, ConnectionGeneration generation) noexcept;

  // Looks up whatever connection currently occupies `id`, ignoring
  // generation. Safe only for the event loop's own epoll-driven dispatch:
  // an epoll event for fd `id` can only refer to whatever this registry
  // currently has registered at that fd, since both are added and removed
  // together on the same (single) thread -- there is no stale-generation
  // hazard to guard against here, unlike a cross-thread work item's (id,
  // generation) pair (see ARCHITECTURE.md, Data Ownership).
  [[nodiscard]] Connection* find_current(ConnectionId id) noexcept;

  [[nodiscard]] std::size_t size() const noexcept { return connections_.size(); }

 private:
  ConnectionGeneration next_generation_ = 1;
  std::unordered_map<ConnectionId, Connection> connections_;
};

}  // namespace kvstore::net
