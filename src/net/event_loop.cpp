#include "kvstore/net/event_loop.hpp"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <string>
#include <system_error>
#include <utility>
#include <variant>

#include "kvstore/net/io.hpp"
#include "kvstore/protocol/parser.hpp"
#include "kvstore/protocol/serializer.hpp"

namespace kvstore::net {

namespace {

constexpr int kMaxEventsPerWait = 128;

[[noreturn]] void throw_errno(const std::string& what) {
  throw std::system_error(errno, std::generic_category(), what);
}

void epoll_add(int epoll_fd, int fd, std::uint32_t events) {
  epoll_event event{};
  event.events = events;
  event.data.fd = fd;
  if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event) != 0) {
    throw_errno("EventLoop: epoll_ctl(ADD) failed");
  }
}

}  // namespace

EventLoop::EventLoop(EventLoopConfig config, worker::BoundedQueue<worker::WorkItem>& work_queue,
                     worker::BoundedQueue<worker::Completion>& completion_queue)
    : config_(std::move(config)),
      listener_(config_.listener),
      epoll_fd_(::epoll_create1(EPOLL_CLOEXEC)),
      wakeup_fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      work_queue_(work_queue),
      completion_queue_(completion_queue) {
  if (!epoll_fd_.valid()) {
    throw_errno("EventLoop: epoll_create1() failed");
  }
  if (!wakeup_fd_.valid()) {
    throw_errno("EventLoop: eventfd() failed");
  }
  epoll_add(epoll_fd_.get(), listener_.fd(), EPOLLIN);
  epoll_add(epoll_fd_.get(), wakeup_fd_.get(), EPOLLIN);

  // Let a worker posting a completion wake a blocked epoll_wait, the same
  // way stop() does, via the same eventfd (ARCHITECTURE.md's "bounded
  // result queue" arrow back into the event loop).
  completion_queue_.set_on_push([this] { wake(); });
}

EventLoop::~EventLoop() {
  // The constructor registered a callback on `completion_queue_` that
  // captures `this`. A WorkerPool sharing that queue may outlive (or be
  // destroyed concurrently with) this EventLoop, and its worker threads
  // could still call push() after this point; clearing the callback first
  // makes that harmless (a no-op) instead of a use-after-free.
  completion_queue_.set_on_push(nullptr);
}

void EventLoop::wake() {
  constexpr std::uint64_t kOne = 1;
  // Best-effort: if this fails the loop is already exiting or the fd is
  // gone, either way nothing further to do from here.
  (void)::write(wakeup_fd_.get(), &kOne, sizeof(kOne));
}

void EventLoop::stop() {
  stop_requested_.store(true, std::memory_order_release);
  wake();
}

void EventLoop::drain_wakeup() {
  std::uint64_t counter = 0;
  while (::read(wakeup_fd_.get(), &counter, sizeof(counter)) > 0) {
    // An eventfd counter can in principle require more than one read to
    // fully drain if writes raced with reads; loop until EAGAIN.
  }
  drain_completions();
}

void EventLoop::run() {
  std::array<epoll_event, kMaxEventsPerWait> events{};

  while (!stop_requested_.load(std::memory_order_acquire)) {
    const int n = ::epoll_wait(epoll_fd_.get(), events.data(), kMaxEventsPerWait, -1);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno("EventLoop: epoll_wait() failed");
    }

    for (int i = 0; i < n; ++i) {
      const int fd = events[static_cast<std::size_t>(i)].data.fd;
      const std::uint32_t flags = events[static_cast<std::size_t>(i)].events;

      if (fd == wakeup_fd_.get()) {
        drain_wakeup();
        continue;
      }
      if (fd == listener_.fd()) {
        handle_listener_readable();
        continue;
      }

      if ((flags & (EPOLLHUP | EPOLLERR)) != 0) {
        if (Connection* connection = connections_.find_current(fd)) {
          close_connection(fd, connection->generation());
        }
        continue;
      }
      if ((flags & EPOLLIN) != 0) {
        handle_connection_readable(fd);
      }
      // The read above may have just closed this connection; re-check
      // before touching it again for the write side of the same event.
      if ((flags & EPOLLOUT) != 0 && connections_.find_current(fd) != nullptr) {
        handle_connection_writable(fd);
      }
    }
  }
}

void EventLoop::handle_listener_readable() {
  for (;;) {
    std::optional<AcceptedClient> accepted = listener_.accept_one();
    if (!accepted.has_value()) {
      return;
    }

    const int client_fd = accepted->fd.get();
    Connection& connection =
        connections_.add(std::move(accepted->fd), std::move(accepted->peer_address),
                         config_.max_input_bytes, config_.max_output_bytes);

    epoll_event event{};
    event.events = EPOLLIN;
    event.data.fd = client_fd;
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, client_fd, &event) != 0) {
      // Could not register with epoll; drop this connection but keep
      // accepting others rather than taking down the listener.
      connections_.close(connection.id(), connection.generation());
    }
  }
}

void EventLoop::handle_connection_readable(ConnectionId id) {
  Connection* connection = connections_.find_current(id);
  if (connection == nullptr) {
    return;
  }
  const ConnectionGeneration generation = connection->generation();

  for (;;) {
    const ReadOutcome outcome = read_into(connection->fd(), connection->input());

    if (outcome.status == ReadStatus::kReadSome) {
      if (!process_input(*connection)) {
        close_connection(id, generation);
        return;
      }
      continue;  // level-triggered: keep draining until EAGAIN
    }
    if (outcome.status == ReadStatus::kWouldBlock) {
      break;
    }
    // kClosed, kBufferFull, or kError all mean this connection cannot
    // continue.
    close_connection(id, generation);
    return;
  }

  // Opportunistically write any responses produced above immediately,
  // rather than always waiting for the next epoll_wait to report EPOLLOUT
  // -- the socket send buffer is almost always immediately writable, so
  // this avoids a needless extra round trip for the common case. Falls
  // back to registering EPOLLOUT interest itself if a write would block.
  handle_connection_writable(id);
}

bool EventLoop::push_responses(Connection& connection, std::vector<std::string> ready) {
  for (std::string& response : ready) {
    if (!connection.output().append(response)) {
      return false;  // output backpressure: cap exceeded (SPEC.md section 5)
    }
  }
  return true;
}

bool EventLoop::process_input(Connection& connection) {
  for (;;) {
    if (connection.sequencer().in_flight_count() >= config_.max_in_flight_per_connection) {
      // Per-connection backpressure: stop parsing further frames from
      // this connection's buffer until an earlier response is released
      // (ARCHITECTURE.md section 4). Already-buffered bytes are left
      // untouched and retried the next time a completion for this
      // connection is processed (see drain_completions()).
      return true;
    }

    const protocol::ParseResult result = protocol::parse_command(connection.input().view());

    if (std::holds_alternative<protocol::NeedMoreData>(result)) {
      return true;
    }

    if (std::holds_alternative<protocol::ParsedCommand>(result)) {
      const auto& parsed = std::get<protocol::ParsedCommand>(result);
      connection.input().consume(parsed.consumed_bytes);
      const std::uint64_t sequence = connection.sequencer().next_request_sequence();

      worker::WorkItem item{.connection_id = connection.id(),
                            .connection_generation = connection.generation(),
                            .sequence = sequence,
                            .command = parsed.command};
      const worker::PushStatus push_status = work_queue_.try_push(item);
      if (push_status == worker::PushStatus::kOk) {
        continue;  // dispatched; keep draining pipelined frames
      }

      // The work queue is full (or closed for shutdown): reply
      // immediately instead of dispatching -- the documented alternative
      // backpressure strategy in ARCHITECTURE.md section 4. Goes through
      // the same sequencer as real completions so ordering is preserved
      // regardless of which path a given response took.
      const std::string busy_response = push_status == worker::PushStatus::kClosed
                                            ? protocol::encode_error("ERR server shutting down")
                                            : protocol::encode_error("ERR server busy, try again");
      if (!push_responses(connection, connection.sequencer().record(sequence, busy_response))) {
        return false;
      }
      continue;
    }

    const auto& error = std::get<protocol::ProtocolError>(result);
    if (error.recoverable) {
      connection.input().consume(error.consumed_bytes);
      const std::uint64_t sequence = connection.sequencer().next_request_sequence();
      if (!push_responses(connection, connection.sequencer().record(
                                          sequence, protocol::encode_error(error.message)))) {
        return false;
      }
      continue;
    }
    // Framing itself is broken; cannot safely resynchronize. Best-effort
    // error reply (still sequenced, so it lands after any earlier
    // still-pending responses), then the caller closes the connection.
    const std::uint64_t sequence = connection.sequencer().next_request_sequence();
    (void)push_responses(
        connection, connection.sequencer().record(sequence, protocol::encode_error(error.message)));
    return false;
  }
}

void EventLoop::handle_connection_writable(ConnectionId id) {
  Connection* connection = connections_.find_current(id);
  if (connection == nullptr) {
    return;
  }
  const ConnectionGeneration generation = connection->generation();

  while (!connection->output().empty()) {
    const WriteOutcome outcome = write_from(connection->fd(), connection->output());
    if (outcome.status == WriteStatus::kWroteSome) {
      continue;
    }
    if (outcome.status == WriteStatus::kWouldBlock) {
      break;
    }
    close_connection(id, generation);
    return;
  }

  update_epoll_interest(*connection);
}

void EventLoop::drain_completions() {
  for (;;) {
    std::optional<worker::Completion> completion = completion_queue_.try_pop();
    if (!completion.has_value()) {
      return;
    }

    // Strict (id, generation) check: unlike the event loop's own
    // epoll-driven dispatch, this completion crossed threads and may
    // refer to a connection that has since closed -- possibly with its fd
    // already reused for an unrelated client (ARCHITECTURE.md, Data
    // Ownership).
    Connection* connection =
        connections_.find(completion->connection_id, completion->connection_generation);
    if (connection == nullptr) {
      continue;  // stale; discard
    }

    std::vector<std::string> ready =
        connection->sequencer().record(completion->sequence, std::move(completion->response));
    if (!push_responses(*connection, std::move(ready))) {
      close_connection(completion->connection_id, completion->connection_generation);
      continue;
    }

    // A response was just released, which may have freed in-flight
    // capacity; retry parsing anything left buffered from this
    // connection that process_input previously deferred.
    if (!process_input(*connection)) {
      close_connection(completion->connection_id, completion->connection_generation);
      continue;
    }

    handle_connection_writable(completion->connection_id);
  }
}

void EventLoop::close_connection(ConnectionId id, ConnectionGeneration generation) {
  Connection* connection = connections_.find(id, generation);
  if (connection == nullptr) {
    return;
  }
  if (!connection->output().empty()) {
    // Best-effort final flush (e.g. a pending error reply); ignore the
    // outcome, we are closing regardless.
    (void)write_from(connection->fd(), connection->output());
  }
  // Ignore failure: EPOLL_CTL_DEL only fails if the fd was already removed
  // (e.g. ENOENT), which is harmless here.
  (void)::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, id, nullptr);
  connections_.close(id, generation);
}

void EventLoop::update_epoll_interest(Connection& connection) {
  epoll_event event{};
  event.events =
      EPOLLIN | (connection.output().empty() ? 0U : static_cast<std::uint32_t>(EPOLLOUT));
  event.data.fd = connection.fd();
  // Ignore failure: the connection may have just been closed by a
  // concurrent path within this same event batch (e.g. EPOLLHUP handling);
  // there is nothing further to do with its epoll registration then.
  (void)::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, connection.fd(), &event);
}

}  // namespace kvstore::net
