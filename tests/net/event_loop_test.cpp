#include "kvstore/net/event_loop.hpp"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "kvstore/net/file_descriptor.hpp"
#include "kvstore/protocol/serializer.hpp"
#include "kvstore/storage/store.hpp"
#include "kvstore/worker/bounded_queue.hpp"
#include "kvstore/worker/work_item.hpp"
#include "kvstore/worker/worker_pool.hpp"

namespace kvstore::net {
namespace {

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
// SO_RCVTIMEO (set by connect_to_loopback) to bound the wait.
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

template <typename T>
std::optional<T> try_pop_with_retry(worker::BoundedQueue<T>& queue,
                                    std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (std::optional<T> item = queue.try_pop()) {
      return item;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return std::nullopt;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

// RAII wrapper for a loop run on its own thread: stops and joins on
// destruction unconditionally, including when a test body exits early via
// a fatal (ASSERT_*) failure. A raw joinable std::thread destroyed on such
// a path would instead call std::terminate().
class RunningLoop {
 public:
  explicit RunningLoop(EventLoop& loop) : loop_(loop), thread_([&loop] { loop.run(); }) {}
  ~RunningLoop() {
    loop_.stop();
    thread_.join();
  }

  RunningLoop(const RunningLoop&) = delete;
  RunningLoop& operator=(const RunningLoop&) = delete;

 private:
  EventLoop& loop_;
  std::thread thread_;
};

class EventLoopTest : public ::testing::Test {
 protected:
  void SetUp() override {
    EventLoopConfig config;
    config.listener.bind_address = "127.0.0.1";
    config.listener.port = 0;
    config.listener.backlog = 16;
    loop_ = std::make_unique<EventLoop>(config, work_queue_, completion_queue_);
    pool_ = std::make_unique<worker::WorkerPool>(2, store_, work_queue_, completion_queue_);
    runner_ = std::thread([this] { loop_->run(); });
  }

  void TearDown() override {
    if (loop_) {
      loop_->stop();
    }
    if (runner_.joinable()) {
      runner_.join();
    }
    // Stop and join every worker before the EventLoop (whose completion
    // queue callback captures `this`) is destroyed.
    pool_.reset();
    loop_.reset();
  }

  storage::Store store_;
  worker::BoundedQueue<worker::WorkItem> work_queue_{64};
  worker::BoundedQueue<worker::Completion> completion_queue_{64};
  std::unique_ptr<worker::WorkerPool> pool_;
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

TEST_F(EventLoopTest, SetThenGetRoundTripsThroughRealStorage) {
  const FileDescriptor client = connect_to_loopback(loop_->port());
  const std::string set_request = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$5\r\nhello\r\n";
  ASSERT_EQ(::send(client.get(), set_request.data(), set_request.size(), 0),
            static_cast<ssize_t>(set_request.size()));
  EXPECT_EQ(recv_exact(client.get(), 5), "+OK\r\n");

  const std::string get_request = "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n";
  ASSERT_EQ(::send(client.get(), get_request.data(), get_request.size(), 0),
            static_cast<ssize_t>(get_request.size()));
  EXPECT_EQ(recv_exact(client.get(), 11), "$5\r\nhello\r\n");
}

TEST_F(EventLoopTest, HandlesARequestFragmentedAcrossMultipleWrites) {
  const FileDescriptor client = connect_to_loopback(loop_->port());
  const std::string request = "*1\r\n$4\r\nPING\r\n";

  const std::size_t first = 3;
  const std::size_t second = 8;
  ASSERT_EQ(::send(client.get(), request.data(), first, 0), static_cast<ssize_t>(first));
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ASSERT_EQ(::send(client.get(), request.data() + first, second - first, 0),
            static_cast<ssize_t>(second - first));
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ASSERT_EQ(::send(client.get(), request.data() + second, request.size() - second, 0),
            static_cast<ssize_t>(request.size() - second));

  EXPECT_EQ(recv_exact(client.get(), 7), "+PONG\r\n");
}

TEST_F(EventLoopTest, HandlesMultiplePipelinedCommandsInOneReadAndPreservesOrder) {
  const FileDescriptor client = connect_to_loopback(loop_->port());
  const std::string request =
      "*3\r\n$3\r\nSET\r\n$1\r\na\r\n$1\r\n1\r\n"
      "*3\r\n$3\r\nSET\r\n$1\r\nb\r\n$1\r\n2\r\n"
      "*2\r\n$3\r\nGET\r\n$1\r\na\r\n"
      "*2\r\n$3\r\nGET\r\n$1\r\nb\r\n";

  ASSERT_EQ(::send(client.get(), request.data(), request.size(), 0),
            static_cast<ssize_t>(request.size()));

  const std::string expected_reply = "+OK\r\n+OK\r\n$1\r\n1\r\n$1\r\n2\r\n";
  EXPECT_EQ(recv_exact(client.get(), expected_reply.size()), expected_reply);
}

TEST_F(EventLoopTest, SurvivesADisconnectAndKeepsServingNewConnections) {
  const std::string request = "*1\r\n$4\r\nPING\r\n";

  {
    const FileDescriptor client = connect_to_loopback(loop_->port());
    ASSERT_EQ(::send(client.get(), request.data(), request.size(), 0),
              static_cast<ssize_t>(request.size()));
    EXPECT_EQ(recv_exact(client.get(), 7), "+PONG\r\n");
  }  // client disconnects here

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
  EXPECT_LT(elapsed, std::chrono::seconds(1));
}

TEST_F(EventLoopTest, HandlesPartialWritesOfALargeResponse) {
  const FileDescriptor client = connect_to_loopback(loop_->port(), /*rcvbuf_bytes=*/4096);
  const std::string big_value(1024 * 1024, 'z');
  const std::string set_request = "*3\r\n$3\r\nSET\r\n$3\r\nbig\r\n$" +
                                  std::to_string(big_value.size()) + "\r\n" + big_value + "\r\n";
  ASSERT_EQ(::send(client.get(), set_request.data(), set_request.size(), 0),
            static_cast<ssize_t>(set_request.size()));
  EXPECT_EQ(recv_exact(client.get(), 5), "+OK\r\n");

  const std::string get_request = "*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n";
  ASSERT_EQ(::send(client.get(), get_request.data(), get_request.size(), 0),
            static_cast<ssize_t>(get_request.size()));

  const std::string expected_reply = protocol::encode_bulk_string(big_value);
  EXPECT_EQ(recv_exact(client.get(), expected_reply.size()), expected_reply);
}

// A work queue with capacity 1 and no draining WorkerPool: the first
// command fills it; a second pipelined command must get a busy reply
// instead of being dispatched. That busy reply is itself sequenced like
// any other response, so it cannot be written ahead of the still-pending
// first response -- releasing the first (as a real worker eventually
// would) must release both, in order, together.
TEST(EventLoopBackpressureTest, WorkQueueFullProducesABusyReplyWithoutDispatching) {
  worker::BoundedQueue<worker::WorkItem> work_queue(1);
  worker::BoundedQueue<worker::Completion> completion_queue(4);

  EventLoopConfig config;
  config.listener.bind_address = "127.0.0.1";
  config.listener.port = 0;
  config.listener.backlog = 16;
  EventLoop loop(config, work_queue, completion_queue);
  RunningLoop running(loop);

  const FileDescriptor client = connect_to_loopback(loop.port());
  const std::string one_ping = "*1\r\n$4\r\nPING\r\n";
  const std::string request = one_ping + one_ping;  // second must overflow the capacity-1 queue
  ASSERT_EQ(::send(client.get(), request.data(), request.size(), 0),
            static_cast<ssize_t>(request.size()));

  const std::optional<worker::WorkItem> first_item =
      try_pop_with_retry(work_queue, std::chrono::seconds(2));
  ASSERT_TRUE(first_item.has_value());

  // The queue is capacity 1 and nothing else drains it, so the second
  // ping must have overflowed it already.
  EXPECT_EQ(work_queue.try_pop(), std::nullopt);

  worker::Completion completion{.connection_id = first_item->connection_id,
                                .connection_generation = first_item->connection_generation,
                                .sequence = first_item->sequence,
                                .response = protocol::encode_simple_string("PONG")};
  ASSERT_EQ(completion_queue.push(std::move(completion)), worker::PushStatus::kOk);

  const std::string expected_reply = "+PONG\r\n-ERR server busy, try again\r\n";
  EXPECT_EQ(recv_exact(client.get(), expected_reply.size()), expected_reply);
}

// max_in_flight_per_connection = 1 with no draining WorkerPool: a second
// pipelined command on the same connection must be left unparsed
// (throttled) rather than queued or answered. Manually completing the
// first request (as a real worker eventually would) must then unblock
// parsing of the second, deterministically proving both the throttle and
// its release.
TEST(EventLoopBackpressureTest, InFlightLimitThrottlesThenReleasesOnCompletion) {
  worker::BoundedQueue<worker::WorkItem> work_queue(8);
  worker::BoundedQueue<worker::Completion> completion_queue(8);

  EventLoopConfig config;
  config.listener.bind_address = "127.0.0.1";
  config.listener.port = 0;
  config.listener.backlog = 16;
  config.max_in_flight_per_connection = 1;
  EventLoop loop(config, work_queue, completion_queue);
  RunningLoop running(loop);

  const FileDescriptor client = connect_to_loopback(loop.port());
  const std::string one_ping = "*1\r\n$4\r\nPING\r\n";
  const std::string request = one_ping + one_ping;
  ASSERT_EQ(::send(client.get(), request.data(), request.size(), 0),
            static_cast<ssize_t>(request.size()));

  const std::optional<worker::WorkItem> item =
      try_pop_with_retry(work_queue, std::chrono::seconds(2));
  ASSERT_TRUE(item.has_value());

  // Exactly one WorkItem should have been enqueued so far (the second
  // frame is throttled, still sitting unparsed in the connection's input
  // buffer) -- confirm nothing is written back yet.
  timeval short_timeout{.tv_sec = 0, .tv_usec = 200'000};
  ::setsockopt(client.get(), SOL_SOCKET, SO_RCVTIMEO, &short_timeout, sizeof(short_timeout));
  char probe[16];
  EXPECT_EQ(::recv(client.get(), probe, sizeof(probe), 0), -1);

  // Manually complete it, exactly as WorkerPool would.
  worker::Completion completion{.connection_id = item->connection_id,
                                .connection_generation = item->connection_generation,
                                .sequence = item->sequence,
                                .response = protocol::encode_simple_string("PONG")};
  ASSERT_EQ(completion_queue.push(std::move(completion)), worker::PushStatus::kOk);

  // Releasing the first response should have unblocked parsing of the
  // second frame, enqueuing it in turn.
  const std::optional<worker::WorkItem> second_item =
      try_pop_with_retry(work_queue, std::chrono::seconds(2));
  ASSERT_TRUE(second_item.has_value());

  worker::Completion second_completion{.connection_id = second_item->connection_id,
                                       .connection_generation = second_item->connection_generation,
                                       .sequence = second_item->sequence,
                                       .response = protocol::encode_simple_string("PONG")};
  ASSERT_EQ(completion_queue.push(std::move(second_completion)), worker::PushStatus::kOk);

  timeval normal_timeout{.tv_sec = 3, .tv_usec = 0};
  ::setsockopt(client.get(), SOL_SOCKET, SO_RCVTIMEO, &normal_timeout, sizeof(normal_timeout));
  EXPECT_EQ(recv_exact(client.get(), 14), "+PONG\r\n+PONG\r\n");
}

}  // namespace
}  // namespace kvstore::net
