#include "kvstore/storage/ttl_sweeper.hpp"

#include <condition_variable>
#include <mutex>
#include <utility>

namespace kvstore::storage {

TtlSweeper::TtlSweeper(Store& store, TtlSweeperConfig config)
    : store_(store),
      config_(config),
      thread_([this](const std::stop_token& token) { run(token); }) {}

TtlSweeper::~TtlSweeper() = default;  // std::jthread requests stop and joins automatically

void TtlSweeper::run(const std::stop_token& token) {
  std::mutex mutex;
  std::condition_variable_any wakeup;

  while (!token.stop_requested()) {
    (void)store_.sweep_expired(config_.max_items_per_shard);

    std::unique_lock lock(mutex);
    // Interruptible sleep: returns early if `token` is stopped, otherwise
    // waits out the full interval before the next pass. The predicate
    // always reports "not yet satisfied" so wait_for only ever returns
    // because of the timeout or the stop request.
    wakeup.wait_for(lock, token, config_.interval, [] { return false; });
  }
}

}  // namespace kvstore::storage
