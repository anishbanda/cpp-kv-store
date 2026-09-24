// Milestone 7: "Add graceful shutdown tests with active clients"
// (TASKS.md) / docs/CONCURRENCY.md #7, "Shutdown Order".
//
// Shuts down a fully wired server (EventLoop + WorkerPool) while clients
// are still connected and mid-request, and checks that this never hangs,
// crashes, or leaves a joinable thread -- exactly the SPEC.md networking
// requirement "Shutdown stops accepting work, drains or cancels according
// to policy, and exits cleanly." Milestone 4/5 already established the
// shutdown order this project uses: stop() the event loop and join its
// thread, then destroy the WorkerPool (closes the work queue so workers
// finish already-queued work before joining), then destroy the EventLoop.

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "kvstore/net/event_loop.hpp"
#include "kvstore/net/file_descriptor.hpp"
#include "kvstore/storage/store.hpp"
#include "kvstore/worker/bounded_queue.hpp"
#include "kvstore/worker/work_item.hpp"
#include "kvstore/worker/worker_pool.hpp"

namespace kvstore::reliability {
namespace {

using net::EventLoop;
using net::EventLoopConfig;
using net::FileDescriptor;

FileDescriptor connect_to_loopback(std::uint16_t port) {
  FileDescriptor fd(::socket(AF_INET, SOCK_STREAM, 0));
  if (!fd.valid()) {
    throw std::runtime_error("connect_to_loopback: socket() failed");
  }
  timeval timeout{.tv_sec = 2, .tv_usec = 0};
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

TEST(ReliabilityTest, GracefulShutdownWithActiveClientsNeverHangsOrCorruptsResponses) {
  storage::Store store;
  worker::BoundedQueue<worker::WorkItem> work_queue(64);
  worker::BoundedQueue<worker::Completion> completion_queue(64);

  EventLoopConfig config;
  config.listener.bind_address = "127.0.0.1";
  config.listener.port = 0;
  config.listener.backlog = 64;
  auto loop = std::make_unique<EventLoop>(config, work_queue, completion_queue);
  auto pool = std::make_unique<worker::WorkerPool>(4, store, work_queue, completion_queue);
  std::thread runner([&loop] { loop->run(); });

  constexpr int kClients = 30;
  std::vector<FileDescriptor> clients;
  clients.reserve(kClients);
  const std::string request = "*1\r\n$4\r\nPING\r\n";
  for (int i = 0; i < kClients; ++i) {
    FileDescriptor client = connect_to_loopback(loop->port());
    ASSERT_EQ(::send(client.get(), request.data(), request.size(), 0),
              static_cast<ssize_t>(request.size()));
    clients.push_back(std::move(client));
  }

  // Deliberately do not wait for (or read) any responses first: shutdown
  // must be safe with requests genuinely still in flight.
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  const auto start = std::chrono::steady_clock::now();
  loop->stop();
  runner.join();
  pool.reset();  // drains already-queued work, then joins every worker
  loop.reset();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_LT(elapsed, std::chrono::seconds(5)) << "shutdown hung instead of completing promptly";

  // Every client must observe either a complete, correct PONG or a clean
  // disconnect -- never a hang, a partial/garbage frame, or a crash.
  for (FileDescriptor& client : clients) {
    char buf[64] = {};
    const ssize_t n = ::recv(client.get(), buf, sizeof(buf), 0);
    if (n > 0) {
      EXPECT_EQ(std::string_view(buf, static_cast<std::size_t>(n)), "+PONG\r\n");
    }
    // n == 0 (orderly close) or n < 0 (e.g. timeout/reset because the
    // server closed before replying) are both acceptable: shutdown is not
    // required to wait for every in-flight response.
  }
}

TEST(ReliabilityTest, ShutdownWithNoConnectionsAtAllIsClean) {
  storage::Store store;
  worker::BoundedQueue<worker::WorkItem> work_queue(8);
  worker::BoundedQueue<worker::Completion> completion_queue(8);

  EventLoopConfig config;
  config.listener.bind_address = "127.0.0.1";
  config.listener.port = 0;
  config.listener.backlog = 8;
  auto loop = std::make_unique<EventLoop>(config, work_queue, completion_queue);
  auto pool = std::make_unique<worker::WorkerPool>(2, store, work_queue, completion_queue);
  std::thread runner([&loop] { loop->run(); });

  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  const auto start = std::chrono::steady_clock::now();
  loop->stop();
  runner.join();
  pool.reset();
  loop.reset();
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_LT(elapsed, std::chrono::seconds(5));
}

}  // namespace
}  // namespace kvstore::reliability
