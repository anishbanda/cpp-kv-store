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

EventLoop::EventLoop(EventLoopConfig config, CommandHandler handler)
    : config_(std::move(config)),
      handler_(std::move(handler)),
      listener_(config_.listener),
      epoll_fd_(::epoll_create1(EPOLL_CLOEXEC)),
      wakeup_fd_(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
  if (!epoll_fd_.valid()) {
    throw_errno("EventLoop: epoll_create1() failed");
  }
  if (!wakeup_fd_.valid()) {
    throw_errno("EventLoop: eventfd() failed");
  }
  epoll_add(epoll_fd_.get(), listener_.fd(), EPOLLIN);
  epoll_add(epoll_fd_.get(), wakeup_fd_.get(), EPOLLIN);
}

EventLoop::~EventLoop() = default;

void EventLoop::stop() {
  stop_requested_.store(true, std::memory_order_release);
  constexpr std::uint64_t kOne = 1;
  // Best-effort: if this fails the loop is already exiting or the fd is
  // gone, either way nothing further to do from a shutdown call.
  (void)::write(wakeup_fd_.get(), &kOne, sizeof(kOne));
}

void EventLoop::drain_wakeup() {
  std::uint64_t counter = 0;
  while (::read(wakeup_fd_.get(), &counter, sizeof(counter)) > 0) {
    // An eventfd counter can in principle require more than one read to
    // fully drain if writes raced with reads; loop until EAGAIN.
  }
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

bool EventLoop::process_input(Connection& connection) {
  for (;;) {
    const protocol::ParseResult result = protocol::parse_command(connection.input().view());

    if (std::holds_alternative<protocol::NeedMoreData>(result)) {
      return true;
    }

    if (std::holds_alternative<protocol::ParsedCommand>(result)) {
      const auto& parsed = std::get<protocol::ParsedCommand>(result);
      std::string response = handler_(parsed.command);
      connection.input().consume(parsed.consumed_bytes);
      if (!connection.output().append(response)) {
        return false;  // output backpressure: cap exceeded (SPEC.md section 5)
      }
      continue;  // keep draining pipelined frames from this same read
    }

    const auto& error = std::get<protocol::ProtocolError>(result);
    if (error.recoverable) {
      connection.input().consume(error.consumed_bytes);
      if (!connection.output().append(protocol::encode_error(error.message))) {
        return false;
      }
      continue;
    }
    // Framing itself is broken; cannot safely resynchronize. Best-effort
    // error reply, then the caller closes the connection.
    (void)connection.output().append(protocol::encode_error(error.message));
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
