#include <pthread.h>

#include <csignal>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "kvstore/cli/options.hpp"
#include "kvstore/net/event_loop.hpp"
#include "kvstore/storage/store.hpp"
#include "kvstore/storage/ttl_sweeper.hpp"
#include "kvstore/util/logger.hpp"
#include "kvstore/version.hpp"
#include "kvstore/worker/bounded_queue.hpp"
#include "kvstore/worker/work_item.hpp"
#include "kvstore/worker/worker_pool.hpp"

namespace {

// Blocks SIGINT/SIGTERM in this thread. Every thread created afterwards
// inherits the calling thread's signal mask, so this must run before any
// other thread is spawned -- it guarantees only the dedicated signal-
// waiting thread below ever receives these signals.
sigset_t block_shutdown_signals() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  ::pthread_sigmask(SIG_BLOCK, &set, nullptr);
  return set;
}

std::string describe_options(const kvstore::cli::ServerOptions& options, std::uint16_t bound_port) {
  return "bind=" + options.bind_address + " port=" + std::to_string(bound_port) +
         " workers=" + std::to_string(options.worker_count) +
         " shards=" + std::to_string(options.shard_count) +
         " queue_capacity=" + std::to_string(options.queue_capacity) +
         " log_level=" + options.log_level;
}

}  // namespace

int main(int argc, char* argv[]) {
  const std::vector<std::string_view> args(argv + 1, argv + argc);

  kvstore::cli::ServerOptions options;
  try {
    options = kvstore::cli::parse_args(args);
  } catch (const std::invalid_argument& e) {
    std::cerr << "error: " << e.what() << "\n\n" << kvstore::cli::usage_text();
    return 1;
  }

  if (options.show_help) {
    std::cout << kvstore::cli::usage_text();
    return 0;
  }
  if (options.show_version) {
    std::cout << "cpp-kv-store " << kvstore::version() << '\n';
    return 0;
  }

  kvstore::util::Logger logger(*kvstore::util::Logger::parse_level(options.log_level));

  // Must happen before any other thread (worker pool, TTL sweeper,
  // signal-waiting thread) is created; see block_shutdown_signals().
  const sigset_t blocked_signals = block_shutdown_signals();

  try {
    logger.info("starting cpp-kv-store " + std::string(kvstore::version()));

    kvstore::storage::Store store(
        kvstore::storage::StoreOptions{.shard_count = options.shard_count});
    kvstore::storage::TtlSweeper sweeper(store);

    kvstore::worker::BoundedQueue<kvstore::worker::WorkItem> work_queue(options.queue_capacity);
    kvstore::worker::BoundedQueue<kvstore::worker::Completion> completion_queue(
        options.queue_capacity);

    kvstore::net::EventLoopConfig event_loop_config;
    event_loop_config.listener.bind_address = options.bind_address;
    event_loop_config.listener.port = options.port;
    event_loop_config.logger = &logger;

    kvstore::net::EventLoop loop(event_loop_config, work_queue, completion_queue);
    kvstore::worker::WorkerPool pool(options.worker_count, store, work_queue, completion_queue);

    logger.info("startup configuration: " + describe_options(options, loop.port()));

    // sigwait() is an ordinary blocking call, not a signal handler, so it
    // has none of the async-signal-safety restrictions
    // docs/CONCURRENCY.md warns about ("A signal handler performs only
    // async-signal-safe notification, such as writing an eventfd"). It
    // only ever calls loop.stop(), documented safe from any thread.
    std::thread signal_thread([&blocked_signals, &loop, &logger] {
      int signal_number = 0;
      ::sigwait(&blocked_signals, &signal_number);
      logger.info("shutdown start: received signal " + std::to_string(signal_number));
      loop.stop();
    });

    // The event loop itself runs on this thread, matching ARCHITECTURE.md
    // ("Runs on one dedicated thread, initially the main thread").
    loop.run();

    // run() only returns after stop() was called, which only happens
    // after sigwait() above already returned -- the signal thread is
    // finished or finishing, so this join cannot block long.
    signal_thread.join();

    // pool, sweeper, loop, the queues, and store all stop and are
    // destroyed here, in reverse declaration order, before control
    // reaches the "shutdown completion" log below.
  } catch (const std::exception& e) {
    logger.error(std::string("fatal error: ") + e.what());
    return 1;
  }

  logger.info("shutdown completion");
  return 0;
}
