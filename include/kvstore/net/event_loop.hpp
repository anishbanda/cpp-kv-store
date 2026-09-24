#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "kvstore/net/connection_registry.hpp"
#include "kvstore/net/file_descriptor.hpp"
#include "kvstore/net/listener.hpp"
#include "kvstore/worker/bounded_queue.hpp"
#include "kvstore/worker/work_item.hpp"

namespace kvstore::net {

struct EventLoopConfig {
  ListenerConfig listener;
  std::size_t max_input_bytes = kDefaultMaxInputBufferBytes;
  std::size_t max_output_bytes = kDefaultMaxOutputBufferBytes;

  // Caps how many requests from one connection may be outstanding
  // (parsed but not yet responded to) at once. Once reached, the event
  // loop stops parsing further frames from that connection's already-
  // buffered input until an earlier response is released -- the
  // "disables read interest for affected connections" backpressure
  // strategy from ARCHITECTURE.md section 4. This is a per-connection
  // limit, distinct from the global work-queue-full case (see below),
  // which instead uses that section's other documented strategy, an
  // immediate busy reply.
  std::size_t max_in_flight_per_connection = 64;
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
// Connection (see ARCHITECTURE.md, Data Ownership). Does not own the
// work/completion queues -- they are shared with a worker::WorkerPool,
// which must outlive this EventLoop, exactly the seam described in
// ARCHITECTURE.md's diagram ("bounded work queue" between the event loop
// and the worker pool, and back via the "bounded result queue"). Not
// thread-safe except for stop(), which is safe to call from another
// thread, and implicitly the queues themselves, which are.
class EventLoop {
 public:
  EventLoop(EventLoopConfig config, worker::BoundedQueue<worker::WorkItem>& work_queue,
            worker::BoundedQueue<worker::Completion>& completion_queue);
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
  // destroyed. The same underlying wakeup also fires when a worker posts
  // a completion (see drain_wakeup()).
  void stop();

 private:
  void handle_listener_readable();
  void handle_connection_readable(ConnectionId id);
  void handle_connection_writable(ConnectionId id);
  // Parses and dispatches (to the work queue) as many complete frames as
  // are currently available, subject to the in-flight cap. Returns false
  // if the connection must be closed (a non-recoverable protocol error,
  // or output backpressure).
  [[nodiscard]] bool process_input(Connection& connection);
  // Appends `ready` (already in request order) to the connection's output
  // buffer. Returns false if the output cap was exceeded (connection must
  // be closed).
  [[nodiscard]] bool push_responses(Connection& connection, std::vector<std::string> ready);
  void close_connection(ConnectionId id, ConnectionGeneration generation);
  void update_epoll_interest(Connection& connection);
  void wake();
  void drain_wakeup();
  void drain_completions();

  EventLoopConfig config_;
  Listener listener_;
  FileDescriptor epoll_fd_;
  FileDescriptor wakeup_fd_;
  ConnectionRegistry connections_;
  worker::BoundedQueue<worker::WorkItem>& work_queue_;
  worker::BoundedQueue<worker::Completion>& completion_queue_;
  std::atomic<bool> stop_requested_{false};
};

}  // namespace kvstore::net
