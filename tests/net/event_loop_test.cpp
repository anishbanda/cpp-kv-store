#include "kvstore/net/event_loop.hpp"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <thread>

#include "kvstore/net/file_descriptor.hpp"
#include "kvstore/protocol/command.hpp"
#include "kvstore/protocol/serializer.hpp"

namespace kvstore::net {
namespace {

// A storage-independent handler used only to exercise the event loop's own
// mechanics (accept/read/parse-including-pipelining/write, EAGAIN, and
// shutdown). Real command semantics arrive with Milestone 5's dispatcher.
std::string echo_handler(const protocol::Command& command) {
  switch (command.type) {
    case protocol::CommandType::kPing:
      return protocol::encode_simple_string("PONG");
    case protocol::CommandType::kGet:
      if (command.key == "big") {
        // Comfortably under the default 2 MiB output buffer cap (see
        // kDefaultMaxOutputBufferBytes) while still far larger than the
        // shrunken receive window HandlesPartialWritesOfALargeResponse
        // uses to force partial, EAGAIN-retried writes.
        static const std::string kBigPayload(1024 * 1024, 'z');
        return protocol::encode_bulk_string(kBigPayload);
      }
      return protocol::encode_bulk_string("value:" + command.key);
    default:
      return protocol::encode_simple_string("OK");
  }
}

FileDescriptor connect_to_loopback(std::uint16_t port, int rcvbuf_bytes = 0) {
  FileDescriptor fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (!fd.valid()) {
    throw std::runtime_error("connect_to_loopback: socket() failed");
  }
  if (rcvbuf_bytes > 0) {
    ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVBUF, &rcvbuf_bytes, sizeof(rcvbuf_bytes));
  }
  timeval timeout{.tv_sec = 5, .tv_usec = 0};
  ::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    throw std::runtime_error("connect_to_loopback: connect() failed");
  }
  return fd;
}

// Accumulates up to `expected_len` bytes, relying on the socket's
// SO_RCVTIMEO (set by connect_to_loopback) to bound the wait. Returns
// whatever was received if the peer closes or the timeout elapses first.
std::string recv_exact(int fd, std::size_t expected_len) {
  std::string result;
  result.reserve(expected_len);
  while (result.size() < expected_len) {
    char buf[65536];
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      break;
    }
    result.append(buf, static_cast<std::size_t>(n));
  }
  return result;
}

template <typename Predicate>
bool wait_until(Predicate predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

class EventLoopTest : public ::testing::Test {
 protected:
  void SetUp() override {
    EventLoopConfig config;
    config.listener.bind_address = "127.0.0.1";
    config.listener.port = 0;
    config.listener.backlog = 16;
    loop_ = std::make_unique<EventLoop>(config, echo_handler);
    runner_ = std::thread([this] { loop_->run(); });
  }

  void TearDown() override {
    if (loop_) {
      loop_->stop();
    }
    if (runner_.joinable()) {
      runner_.join();
    }
  }

  std::unique_ptr<EventLoop> loop_;
  std::thread runner_;
};

TEST_F(EventLoopTest, RespondsToASingleCommand) {
  const FileDescriptor client = connect_to_loopback(loop_->port());
  const std::string request = "*1\r\n$4\r\nPING\r\n";
  ASSERT_EQ(::send(client.get(), request.data(), request.size(), 0),
            static_cast<ssize_t>(request.size()));

  const std::string reply = recv_exact(client.get(), 7);
  EXPECT_EQ(reply, "+PONG\r\n");
}

TEST_F(EventLoopTest, HandlesARequestFragmentedAcrossMultipleWrites) {
  const FileDescriptor client = connect_to_loopback(loop_->port());
  const std::string request = "*1\r\n$4\r\nPING\r\n";

  // Split into three pieces, each sent separately with a small pause so
  // the server is likely to see them as distinct reads -- exercising
  // NeedMoreData followed by eventual completion.
  const std::size_t first = 3;
  const std::size_t second = 8;
  ASSERT_EQ(::send(client.get(), request.data(), first, 0), static_cast<ssize_t>(first));
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ASSERT_EQ(::send(client.get(), request.data() + first, second - first, 0),
            static_cast<ssize_t>(second - first));
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ASSERT_EQ(::send(client.get(), request.data() + second, request.size() - second, 0),
            static_cast<ssize_t>(request.size() - second));

  const std::string reply = recv_exact(client.get(), 7);
  EXPECT_EQ(reply, "+PONG\r\n");
}

TEST_F(EventLoopTest, HandlesMultiplePipelinedCommandsInOneRead) {
  const FileDescriptor client = connect_to_loopback(loop_->port());
  const std::string one_ping = "*1\r\n$4\r\nPING\r\n";
  const std::string request = one_ping + one_ping + one_ping;

  ASSERT_EQ(::send(client.get(), request.data(), request.size(), 0),
            static_cast<ssize_t>(request.size()));

  const std::string expected_reply = "+PONG\r\n+PONG\r\n+PONG\r\n";
  const std::string reply = recv_exact(client.get(), expected_reply.size());
  EXPECT_EQ(reply, expected_reply);
}

TEST_F(EventLoopTest, HandlesMixedPipelinedCommands) {
  const FileDescriptor client = connect_to_loopback(loop_->port());
  const std::string request =
      "*1\r\n$4\r\nPING\r\n"
      "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n";

  ASSERT_EQ(::send(client.get(), request.data(), request.size(), 0),
            static_cast<ssize_t>(request.size()));

  const std::string expected_reply = "+PONG\r\n" + protocol::encode_bulk_string("value:k");
  const std::string reply = recv_exact(client.get(), expected_reply.size());
  EXPECT_EQ(reply, expected_reply);
}

TEST_F(EventLoopTest, SurvivesADisconnectAndKeepsServingNewConnections) {
  const std::string request = "*1\r\n$4\r\nPING\r\n";

  {
    const FileDescriptor client = connect_to_loopback(loop_->port());
    ASSERT_EQ(::send(client.get(), request.data(), request.size(), 0),
              static_cast<ssize_t>(request.size()));
    EXPECT_EQ(recv_exact(client.get(), 7), "+PONG\r\n");
  }  // client disconnects here

  // Give the loop a moment to observe and process the disconnect (an
  // epoll-driven, asynchronous event from the test's point of view) before
  // proving it is still healthy.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  const FileDescriptor second_client = connect_to_loopback(loop_->port());
  ASSERT_EQ(::send(second_client.get(), request.data(), request.size(), 0),
            static_cast<ssize_t>(request.size()));
  EXPECT_EQ(recv_exact(second_client.get(), 7), "+PONG\r\n");
}

TEST_F(EventLoopTest, StopWakesTheLoopPromptly) {
  const auto start = std::chrono::steady_clock::now();
  loop_->stop();
  runner_.join();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  // run() was blocked in epoll_wait(-1); a bound well under its timeout
  // (which does not exist) proves the eventfd wakeup fired, not that
  // something else happened to return first.
  EXPECT_LT(elapsed, std::chrono::seconds(1));
}

TEST_F(EventLoopTest, HandlesPartialWritesOfALargeResponse) {
  // A small receive buffer constrains the TCP window so a several-MiB
  // response cannot be written by the server in one nonblocking send();
  // the event loop must fall back to EPOLLOUT-driven writes to finish it.
  const FileDescriptor client = connect_to_loopback(loop_->port(), /*rcvbuf_bytes=*/4096);
  const std::string request = "*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n";
  ASSERT_EQ(::send(client.get(), request.data(), request.size(), 0),
            static_cast<ssize_t>(request.size()));

  const std::string big_payload(1024 * 1024, 'z');
  const std::string expected_reply = protocol::encode_bulk_string(big_payload);

  const std::string reply = recv_exact(client.get(), expected_reply.size());
  EXPECT_EQ(reply, expected_reply);
}

}  // namespace
}  // namespace kvstore::net
