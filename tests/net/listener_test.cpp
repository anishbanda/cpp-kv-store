#include "kvstore/net/listener.hpp"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

#include "kvstore/net/bounded_buffer.hpp"
#include "kvstore/net/connection_registry.hpp"
#include "kvstore/net/file_descriptor.hpp"
#include "kvstore/net/io.hpp"

namespace kvstore::net {
namespace {

constexpr std::chrono::milliseconds kRetryTimeout{2000};
constexpr std::chrono::milliseconds kRetryInterval{1};

// A plain blocking client socket, standing in for a real TCP client. This
// is test-only: production networking code is always nonblocking.
FileDescriptor connect_to_loopback(std::uint16_t port) {
  FileDescriptor fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (!fd.valid()) {
    throw std::runtime_error("connect_to_loopback: socket() failed");
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    throw std::runtime_error("connect_to_loopback: connect() failed");
  }
  return fd;
}

std::optional<AcceptedClient> accept_with_retry(Listener& listener) {
  const auto deadline = std::chrono::steady_clock::now() + kRetryTimeout;
  for (;;) {
    if (std::optional<AcceptedClient> client = listener.accept_one()) {
      return client;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return std::nullopt;
    }
    std::this_thread::sleep_for(kRetryInterval);
  }
}

// Reads until `read_into` reports something other than kWouldBlock, or the
// timeout elapses. Returns the outcome of that final call.
ReadOutcome read_with_retry(int fd, BoundedBuffer& buffer) {
  const auto deadline = std::chrono::steady_clock::now() + kRetryTimeout;
  for (;;) {
    const ReadOutcome outcome = read_into(fd, buffer);
    if (outcome.status != ReadStatus::kWouldBlock) {
      return outcome;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return outcome;
    }
    std::this_thread::sleep_for(kRetryInterval);
  }
}

TEST(ListenerTest, EphemeralPortIsReportedAfterBind) {
  const Listener listener(ListenerConfig{.bind_address = "127.0.0.1", .port = 0, .backlog = 16});
  EXPECT_NE(listener.port(), 0);
}

TEST(ListenerTest, AcceptsALoopbackConnection) {
  Listener listener(ListenerConfig{.bind_address = "127.0.0.1", .port = 0, .backlog = 16});
  const FileDescriptor client = connect_to_loopback(listener.port());

  const std::optional<AcceptedClient> accepted = accept_with_retry(listener);
  ASSERT_TRUE(accepted.has_value());
  EXPECT_TRUE(accepted->fd.valid());
  EXPECT_FALSE(accepted->peer_address.empty());
  EXPECT_TRUE(client.valid());
}

TEST(ListenerTest, AcceptOneReturnsNulloptWhenNothingIsPending) {
  Listener listener(ListenerConfig{.bind_address = "127.0.0.1", .port = 0, .backlog = 16});
  EXPECT_EQ(listener.accept_one(), std::nullopt);
}

TEST(ListenerTest, MultipleConnectionsGetDistinctIdsAndGenerations) {
  Listener listener(ListenerConfig{.bind_address = "127.0.0.1", .port = 0, .backlog = 16});
  const FileDescriptor client_a = connect_to_loopback(listener.port());
  const FileDescriptor client_b = connect_to_loopback(listener.port());

  ConnectionRegistry registry;
  std::optional<AcceptedClient> accepted_a = accept_with_retry(listener);
  std::optional<AcceptedClient> accepted_b = accept_with_retry(listener);
  ASSERT_TRUE(accepted_a.has_value());
  ASSERT_TRUE(accepted_b.has_value());

  const Connection& a = registry.add(std::move(accepted_a->fd), accepted_a->peer_address);
  const Connection& b = registry.add(std::move(accepted_b->fd), accepted_b->peer_address);

  EXPECT_NE(a.id(), b.id());
  EXPECT_NE(a.generation(), b.generation());
  EXPECT_EQ(registry.size(), 2u);
}

TEST(ListenerTest, BasicRequestResponseOnLoopback) {
  Listener listener(ListenerConfig{.bind_address = "127.0.0.1", .port = 0, .backlog = 16});
  const FileDescriptor client = connect_to_loopback(listener.port());

  std::optional<AcceptedClient> accepted = accept_with_retry(listener);
  ASSERT_TRUE(accepted.has_value());

  ConnectionRegistry registry;
  Connection& server_side = registry.add(std::move(accepted->fd), accepted->peer_address);

  // Client sends a request.
  const std::string request = "PING\r\n";
  ASSERT_EQ(::send(client.get(), request.data(), request.size(), 0),
            static_cast<ssize_t>(request.size()));

  // Server reads it into the connection's bounded input buffer.
  const ReadOutcome read_outcome = read_with_retry(server_side.fd(), server_side.input());
  ASSERT_EQ(read_outcome.status, ReadStatus::kReadSome);
  EXPECT_EQ(server_side.input().view(), request);
  server_side.input().consume(server_side.input().size());

  // Server writes a reply through the connection's bounded output buffer.
  const std::string reply = "+PONG\r\n";
  ASSERT_TRUE(server_side.output().append(reply));
  const WriteOutcome write_outcome = write_from(server_side.fd(), server_side.output());
  ASSERT_EQ(write_outcome.status, WriteStatus::kWroteSome);
  EXPECT_EQ(write_outcome.bytes_written, reply.size());
  EXPECT_TRUE(server_side.output().empty());

  // Client reads the reply.
  char received[64] = {};
  const ssize_t n = ::recv(client.get(), received, sizeof(received), 0);
  ASSERT_EQ(n, static_cast<ssize_t>(reply.size()));
  EXPECT_EQ(std::string_view(received, static_cast<std::size_t>(n)), reply);
}

TEST(ListenerTest, ServerSeesClosedWhenClientDisconnects) {
  Listener listener(ListenerConfig{.bind_address = "127.0.0.1", .port = 0, .backlog = 16});
  FileDescriptor client = connect_to_loopback(listener.port());

  std::optional<AcceptedClient> accepted = accept_with_retry(listener);
  ASSERT_TRUE(accepted.has_value());

  ConnectionRegistry registry;
  Connection& server_side = registry.add(std::move(accepted->fd), accepted->peer_address);
  const ConnectionId id = server_side.id();
  const ConnectionGeneration generation = server_side.generation();

  client.reset();  // client disconnects

  BoundedBuffer scratch(1024);
  const ReadOutcome outcome = read_with_retry(server_side.fd(), scratch);
  EXPECT_EQ(outcome.status, ReadStatus::kClosed);

  EXPECT_TRUE(registry.close(id, generation));
  EXPECT_EQ(registry.size(), 0u);
}

TEST(ListenerTest, RejectsInvalidBindAddress) {
  EXPECT_THROW(Listener(ListenerConfig{.bind_address = "not-an-address", .port = 0, .backlog = 16}),
               std::invalid_argument);
}

TEST(ListenerTest, RejectsNonPositiveBacklog) {
  EXPECT_THROW(Listener(ListenerConfig{.bind_address = "127.0.0.1", .port = 0, .backlog = 0}),
               std::invalid_argument);
}

}  // namespace
}  // namespace kvstore::net
