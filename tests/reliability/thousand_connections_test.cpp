// Milestone 7: SPEC.md's networking acceptance criterion --
// "A scripted test creates 1,000 concurrent client connections without
// server failure" -- exercised against the real, fully wired server
// (EventLoop + WorkerPool + Store), not a mock.

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
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

std::string recv_exact(int fd, std::size_t expected_len) {
  std::string result;
  result.reserve(expected_len);
  while (result.size() < expected_len) {
    char buf[4096];
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
      break;
    }
    result.append(buf, static_cast<std::size_t>(n));
  }
  return result;
}

// Raises this process's open-file soft limit toward its hard limit so
// ~2,000 simultaneous sockets (1,000 client-side + 1,000 server-accepted,
// plus the listener/epoll/test-harness overhead) fit comfortably. Returns
// the resulting soft limit.
rlim_t raise_fd_limit() {
  rlimit limit{};
  if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) {
    return 0;
  }
  limit.rlim_cur = std::min<rlim_t>(limit.rlim_max, 8192);
  ::setrlimit(RLIMIT_NOFILE, &limit);
  ::getrlimit(RLIMIT_NOFILE, &limit);
  return limit.rlim_cur;
}

TEST(ReliabilityTest, HandlesOneThousandConcurrentConnections) {
  ASSERT_GE(raise_fd_limit(), 2500u)
      << "raise the shell's `ulimit -n` (or the container's) before running this test";

  storage::Store store;
  worker::BoundedQueue<worker::WorkItem> work_queue(2048);
  worker::BoundedQueue<worker::Completion> completion_queue(2048);

  EventLoopConfig config;
  config.listener.bind_address = "127.0.0.1";
  config.listener.port = 0;
  config.listener.backlog = 1024;
  EventLoop loop(config, work_queue, completion_queue);
  worker::WorkerPool pool(4, store, work_queue, completion_queue);
  std::thread runner([&loop] { loop.run(); });

  constexpr int kConnections = 1000;
  std::vector<FileDescriptor> clients;
  clients.reserve(kConnections);
  for (int i = 0; i < kConnections; ++i) {
    clients.push_back(connect_to_loopback(loop.port()));
  }
  EXPECT_EQ(clients.size(), static_cast<std::size_t>(kConnections));

  // Every connection round-trips a real request, proving the server
  // services all of them concurrently, not merely accepts them.
  const std::string request = "*1\r\n$4\r\nPING\r\n";
  for (FileDescriptor& client : clients) {
    ASSERT_EQ(::send(client.get(), request.data(), request.size(), 0),
              static_cast<ssize_t>(request.size()));
  }

  int successes = 0;
  for (FileDescriptor& client : clients) {
    if (recv_exact(client.get(), 7) == "+PONG\r\n") {
      ++successes;
    }
  }
  EXPECT_EQ(successes, kConnections);

  clients.clear();  // disconnect all 1,000 at once
  loop.stop();
  runner.join();
}

}  // namespace
}  // namespace kvstore::reliability
