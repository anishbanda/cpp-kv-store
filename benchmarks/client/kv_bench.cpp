// A minimal, standalone RESP2 benchmark client for this project's seven
// commands. Deliberately not linked against kvstore_core: it speaks the
// wire protocol directly, the same way any external client would, so it
// exercises the server exactly as a real client sees it.
//
// Closed-loop model: each connection has at most one request in flight,
// sending the next request only after the previous reply is fully read.
// This yields accurate per-request latency at the cost of not probing
// maximum pipelined throughput -- an intentional, documented choice (see
// benchmarks/README.md) appropriate for reporting p50/p95/p99 latency
// alongside throughput, per TASKS.md.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct Options {
  std::string host = "127.0.0.1";
  std::uint16_t port = 6380;
  int connections = 50;
  double duration_seconds = 5.0;
  std::string workload = "get";  // get | set | mixed8020 | mixed5050
  std::size_t value_size = 64;
  std::size_t keyspace = 10000;
  bool csv = false;
};

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error(message); }

Options parse_args(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto next = [&]() -> std::string_view {
      if (i + 1 >= argc) {
        fail("missing value for " + std::string(arg));
      }
      return argv[++i];
    };
    if (arg == "--host") {
      options.host = std::string(next());
    } else if (arg == "--port") {
      options.port = static_cast<std::uint16_t>(std::stoi(std::string(next())));
    } else if (arg == "--connections") {
      options.connections = std::stoi(std::string(next()));
    } else if (arg == "--duration") {
      options.duration_seconds = std::stod(std::string(next()));
    } else if (arg == "--workload") {
      options.workload = std::string(next());
    } else if (arg == "--value-size") {
      options.value_size = static_cast<std::size_t>(std::stoul(std::string(next())));
    } else if (arg == "--keyspace") {
      options.keyspace = static_cast<std::size_t>(std::stoul(std::string(next())));
    } else if (arg == "--csv") {
      options.csv = true;
    } else {
      fail("unknown option: " + std::string(arg));
    }
  }
  if (options.connections <= 0) {
    fail("--connections must be positive");
  }
  if (options.workload != "get" && options.workload != "set" && options.workload != "mixed8020" &&
      options.workload != "mixed5050") {
    fail("--workload must be one of: get, set, mixed8020, mixed5050");
  }
  return options;
}

int connect_blocking(const std::string& host, std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    fail("socket() failed");
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    ::close(fd);
    fail("invalid --host address: " + host);
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    fail("connect() to " + host + " failed");
  }
  constexpr int kEnable = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &kEnable, sizeof(kEnable));
  return fd;
}

std::string encode_command(std::initializer_list<std::string_view> parts) {
  std::string out = "*" + std::to_string(parts.size()) + "\r\n";
  for (const std::string_view part : parts) {
    out += "$" + std::to_string(part.size()) + "\r\n";
    out.append(part);
    out += "\r\n";
  }
  return out;
}

// Sends `request`, then reads until exactly one complete RESP2 reply has
// arrived (simple string, error, integer, bulk string, or null bulk
// string -- the five types this server ever sends). The reply's content
// is discarded; only whether it completed matters for benchmarking.
bool send_and_receive(int fd, const std::string& request, std::string& buffer) {
  std::size_t sent = 0;
  while (sent < request.size()) {
    const ssize_t n = ::send(fd, request.data() + sent, request.size() - sent, 0);
    if (n <= 0) {
      return false;
    }
    sent += static_cast<std::size_t>(n);
  }

  buffer.clear();
  for (;;) {
    char chunk[4096];
    const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      return false;
    }
    buffer.append(chunk, static_cast<std::size_t>(n));
    if (buffer.empty()) {
      continue;
    }

    const char type = buffer.front();
    if (type == '+' || type == '-' || type == ':') {
      if (buffer.find("\r\n") != std::string::npos) {
        return true;
      }
      continue;
    }
    if (type == '$') {
      const std::size_t header_end = buffer.find("\r\n");
      if (header_end == std::string::npos) {
        continue;
      }
      const long length = std::strtol(buffer.c_str() + 1, nullptr, 10);
      if (length < 0) {
        return true;  // null bulk string: "$-1\r\n"
      }
      const std::size_t total = header_end + 2 + static_cast<std::size_t>(length) + 2;
      if (buffer.size() >= total) {
        return true;
      }
      continue;
    }
    return false;  // not one of the five reply types this server sends
  }
}

std::string bench_key(std::size_t index) { return "bench:" + std::to_string(index); }

void populate_keyspace(const Options& options) {
  const int fd = connect_blocking(options.host, options.port);
  const std::string value(options.value_size, 'x');
  std::string buffer;
  for (std::size_t i = 0; i < options.keyspace; ++i) {
    const std::string request = encode_command({"SET", bench_key(i), value});
    if (!send_and_receive(fd, request, buffer)) {
      ::close(fd);
      fail("failed to pre-populate keyspace");
    }
  }
  ::close(fd);
}

struct ThreadResult {
  std::uint64_t requests = 0;
  std::uint64_t errors = 0;
  std::vector<double> latencies_us;
};

ThreadResult run_connection(const Options& options, int thread_index,
                            const std::atomic<bool>& stop) {
  ThreadResult result;
  const int fd = connect_blocking(options.host, options.port);

  std::mt19937 rng(static_cast<unsigned>(thread_index) + 1);
  std::uniform_int_distribution<std::size_t> key_dist(0, options.keyspace - 1);
  std::uniform_int_distribution<int> mix_dist(0, 99);
  const std::string value(options.value_size, 'x');
  std::string buffer;

  while (!stop.load(std::memory_order_relaxed)) {
    const std::string key = bench_key(key_dist(rng));

    bool do_set = false;
    if (options.workload == "set") {
      do_set = true;
    } else if (options.workload == "mixed8020") {
      do_set = mix_dist(rng) < 20;  // 80% GET, 20% SET
    } else if (options.workload == "mixed5050") {
      do_set = mix_dist(rng) < 50;
    }

    const std::string request =
        do_set ? encode_command({"SET", key, value}) : encode_command({"GET", key});

    const auto start = std::chrono::steady_clock::now();
    const bool ok = send_and_receive(fd, request, buffer);
    const auto end = std::chrono::steady_clock::now();

    if (!ok) {
      ++result.errors;
      break;
    }
    ++result.requests;
    result.latencies_us.push_back(std::chrono::duration<double, std::micro>(end - start).count());
  }

  ::close(fd);
  return result;
}

double percentile(const std::vector<double>& sorted_latencies_us, double p) {
  if (sorted_latencies_us.empty()) {
    return 0.0;
  }
  const auto index =
      static_cast<std::size_t>(p * static_cast<double>(sorted_latencies_us.size() - 1));
  return sorted_latencies_us[index];
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Options options = parse_args(argc, argv);

    populate_keyspace(options);

    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    std::vector<ThreadResult> results(static_cast<std::size_t>(options.connections));

    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < options.connections; ++i) {
      threads.emplace_back(
          [&, i] { results[static_cast<std::size_t>(i)] = run_connection(options, i, stop); });
    }

    std::this_thread::sleep_for(std::chrono::duration<double>(options.duration_seconds));
    stop.store(true, std::memory_order_relaxed);

    for (std::thread& thread : threads) {
      thread.join();
    }
    const auto end = std::chrono::steady_clock::now();
    const double elapsed_seconds = std::chrono::duration<double>(end - start).count();

    std::uint64_t total_requests = 0;
    std::uint64_t total_errors = 0;
    std::vector<double> all_latencies_us;
    for (const ThreadResult& result : results) {
      total_requests += result.requests;
      total_errors += result.errors;
      all_latencies_us.insert(all_latencies_us.end(), result.latencies_us.begin(),
                              result.latencies_us.end());
    }
    std::sort(all_latencies_us.begin(), all_latencies_us.end());

    const double throughput =
        elapsed_seconds > 0.0 ? static_cast<double>(total_requests) / elapsed_seconds : 0.0;
    const double p50 = percentile(all_latencies_us, 0.50);
    const double p95 = percentile(all_latencies_us, 0.95);
    const double p99 = percentile(all_latencies_us, 0.99);

    if (options.csv) {
      std::cout << options.connections << ',' << options.workload << ',' << throughput << ',' << p50
                << ',' << p95 << ',' << p99 << ',' << total_requests << ',' << total_errors << ','
                << elapsed_seconds << '\n';
    } else {
      std::cout << "connections=" << options.connections << " workload=" << options.workload
                << " requests=" << total_requests << " errors=" << total_errors
                << " duration_s=" << elapsed_seconds << " throughput_rps=" << throughput
                << " p50_us=" << p50 << " p95_us=" << p95 << " p99_us=" << p99 << '\n';
    }
    return total_errors > 0 ? 2 : 0;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << '\n';
    return 1;
  }
}
