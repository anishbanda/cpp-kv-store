#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "kvstore/net/file_descriptor.hpp"

namespace kvstore::net {

struct ListenerConfig {
  std::string bind_address = "0.0.0.0";  // IPv4 dotted-decimal; SPEC.md default
  std::uint16_t port = 6380;             // 0 requests an OS-assigned ephemeral port
  int backlog = 128;
};

struct AcceptedClient {
  FileDescriptor fd;
  std::string peer_address;  // "a.b.c.d:port", for logging
};

// A nonblocking IPv4 TCP listener. Owns exactly one listening socket.
// Intended to be driven by the event-loop thread (Milestone 4); this class
// does no locking and is not safe to share across threads.
class Listener {
 public:
  explicit Listener(const ListenerConfig& config);

  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;
  Listener(Listener&&) = default;
  Listener& operator=(Listener&&) = default;

  [[nodiscard]] int fd() const noexcept { return fd_.get(); }

  // The actual bound port. Matches `config.port` unless it was 0, in which
  // case this returns the port the OS assigned (mainly useful for tests).
  [[nodiscard]] std::uint16_t port() const;

  // Accepts a single pending connection, if any. Nonblocking: returns
  // std::nullopt when none is currently pending (EAGAIN/EWOULDBLOCK) --
  // callers drive this in a loop until it returns nullopt (SPEC.md
  // section: "Accepts connections until accept4 returns EAGAIN").
  [[nodiscard]] std::optional<AcceptedClient> accept_one();

 private:
  FileDescriptor fd_;
};

}  // namespace kvstore::net
