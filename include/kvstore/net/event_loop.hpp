#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "kvstore/net/connection_registry.hpp"
#include "kvstore/net/file_descriptor.hpp"
#include "kvstore/net/listener.hpp"
#include "kvstore/protocol/command.hpp"

namespace kvstore::net {

// Given a fully parsed command, synchronously returns the RESP-encoded
// response bytes to write back.
//
// This is a Milestone 4 placeholder seam: it lets the event loop (accept,
// read, RESP framing including pipelining, write, epoll mechanics,
// shutdown) be built and tested end-to-end without depending on the
// worker pool or the storage engine, neither of which exists yet --
// TASKS.md assigns "dispatch all seven commands to storage" to Milestone
// 5. The event loop calls this in-line, on the event-loop thread, once
// per parsed command. Milestone 5 replaces synchronous in-line handling
// with bounded work/completion queues and a worker pool; the read/parse/
// write logic in this class should not need to change.
using CommandHandler = std::function<std::string(const protocol::Command&)>;

struct EventLoopConfig {
  ListenerConfig listener;
  std::size_t max_input_bytes = kDefaultMaxInputBufferBytes;
  std::size_t max_output_bytes = kDefaultMaxOutputBufferBytes;
};

// A single-threaded, level-triggered epoll reactor.
//
// Level-triggered (not edge-triggered) is the documented choice for
// Version 1: it tolerates handlers that don't fully drain a socket on
// every wakeup (epoll simply reports it again), which is simpler to get
// right than edge-triggered's requirement to loop a fd to EAGAIN on every
// event or silently miss data. The trade-off is a possible extra wakeup
// when a handler intentionally stops early; revisit only if Milestone 9
// profiling shows this matters.
//
// Owns the epoll descriptor, the listening socket, and every accepted
// Connection (see ARCHITECTURE.md, Data Ownership). Not thread-safe
// except for stop(), which is safe to call from another thread.
class EventLoop {
 public:
  EventLoop(EventLoopConfig config, CommandHandler handler);
  ~EventLoop();

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;
  EventLoop(EventLoop&&) = delete;
  EventLoop& operator=(EventLoop&&) = delete;

  [[nodiscard]] std::uint16_t port() const { return listener_.port(); }

  // Runs until stop() is called or a fatal (non-EINTR) epoll_wait error
  // occurs. Blocks the calling thread; intended to be the event-loop
  // thread's entire body (ARCHITECTURE.md: "one dedicated thread").
  void run();

  // Thread-safe: wakes a blocked run() and asks it to stop once the
  // current batch of events has been handled. Open connections are not
  // forcibly closed by stop() itself -- they close when this EventLoop is
  // destroyed.
  void stop();

 private:
  void handle_listener_readable();
  void handle_connection_readable(ConnectionId id);
  void handle_connection_writable(ConnectionId id);
  // Returns false if the connection must be closed (a non-recoverable
  // protocol error, or output backpressure).
  [[nodiscard]] bool process_input(Connection& connection);
  void close_connection(ConnectionId id, ConnectionGeneration generation);
  void update_epoll_interest(Connection& connection);
  void drain_wakeup();

  EventLoopConfig config_;
  CommandHandler handler_;
  Listener listener_;
  FileDescriptor epoll_fd_;
  FileDescriptor wakeup_fd_;
  ConnectionRegistry connections_;
  std::atomic<bool> stop_requested_{false};
};

}  // namespace kvstore::net
