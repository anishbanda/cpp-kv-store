#include "kvstore/net/io.hpp"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "kvstore/net/bounded_buffer.hpp"
#include "kvstore/net/file_descriptor.hpp"

namespace kvstore::net {
namespace {

struct SocketPair {
  FileDescriptor a;
  FileDescriptor b;
};

SocketPair make_socket_pair() {
  int fds[2];
  EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds), 0);
  return SocketPair{.a = FileDescriptor(fds[0]), .b = FileDescriptor(fds[1])};
}

TEST(IoTest, ReadIntoReturnsWouldBlockWhenNothingIsPending) {
  const SocketPair pair = make_socket_pair();
  BoundedBuffer buffer(1024);

  const ReadOutcome outcome = read_into(pair.a.get(), buffer);
  EXPECT_EQ(outcome.status, ReadStatus::kWouldBlock);
  EXPECT_EQ(outcome.bytes_read, 0u);
  EXPECT_TRUE(buffer.empty());
}

TEST(IoTest, ReadIntoReadsAvailableBytes) {
  const SocketPair pair = make_socket_pair();
  ASSERT_EQ(::send(pair.b.get(), "hello", 5, 0), 5);

  BoundedBuffer buffer(1024);
  const ReadOutcome outcome = read_into(pair.a.get(), buffer);

  EXPECT_EQ(outcome.status, ReadStatus::kReadSome);
  EXPECT_EQ(outcome.bytes_read, 5u);
  EXPECT_EQ(buffer.view(), "hello");
}

TEST(IoTest, ReadIntoAccumulatesAcrossMultipleCalls) {
  const SocketPair pair = make_socket_pair();
  BoundedBuffer buffer(1024);

  ASSERT_EQ(::send(pair.b.get(), "foo", 3, 0), 3);
  ASSERT_EQ(read_into(pair.a.get(), buffer).status, ReadStatus::kReadSome);
  ASSERT_EQ(::send(pair.b.get(), "bar", 3, 0), 3);
  ASSERT_EQ(read_into(pair.a.get(), buffer).status, ReadStatus::kReadSome);

  EXPECT_EQ(buffer.view(), "foobar");
}

TEST(IoTest, ReadIntoReportsClosedOnOrderlyShutdown) {
  SocketPair pair = make_socket_pair();
  pair.b.reset();  // closes b's end

  BoundedBuffer buffer(1024);
  const ReadOutcome outcome = read_into(pair.a.get(), buffer);
  EXPECT_EQ(outcome.status, ReadStatus::kClosed);
}

TEST(IoTest, ReadIntoReportsBufferFullWithoutReading) {
  const SocketPair pair = make_socket_pair();
  ASSERT_EQ(::send(pair.b.get(), "data", 4, 0), 4);

  BoundedBuffer buffer(1);
  ASSERT_TRUE(buffer.append("x"));  // buffer already at capacity

  const ReadOutcome outcome = read_into(pair.a.get(), buffer);
  EXPECT_EQ(outcome.status, ReadStatus::kBufferFull);
  EXPECT_EQ(outcome.bytes_read, 0u);
  EXPECT_EQ(buffer.view(), "x");  // unchanged; the pending "data" is still unread on the socket
}

TEST(IoTest, WriteFromOnEmptyBufferIsANoOp) {
  const SocketPair pair = make_socket_pair();
  BoundedBuffer buffer(1024);

  const WriteOutcome outcome = write_from(pair.a.get(), buffer);
  EXPECT_EQ(outcome.status, WriteStatus::kWroteSome);
  EXPECT_EQ(outcome.bytes_written, 0u);
}

TEST(IoTest, WriteFromWritesAndConsumesPendingBytes) {
  const SocketPair pair = make_socket_pair();
  BoundedBuffer buffer(1024);
  ASSERT_TRUE(buffer.append("hello"));

  const WriteOutcome outcome = write_from(pair.a.get(), buffer);
  EXPECT_EQ(outcome.status, WriteStatus::kWroteSome);
  EXPECT_EQ(outcome.bytes_written, 5u);
  EXPECT_TRUE(buffer.empty());

  char received[16] = {};
  const ssize_t n = ::recv(pair.b.get(), received, sizeof(received), 0);
  ASSERT_EQ(n, 5);
  EXPECT_EQ(std::string_view(received, 5), "hello");
}

TEST(IoTest, WriteFromReportsClosedWhenPeerHasHungUp) {
  SocketPair pair = make_socket_pair();
  pair.b.reset();

  BoundedBuffer buffer(1024);
  ASSERT_TRUE(buffer.append("data"));

  const WriteOutcome outcome = write_from(pair.a.get(), buffer);
  EXPECT_EQ(outcome.status, WriteStatus::kClosed);
}

}  // namespace
}  // namespace kvstore::net
