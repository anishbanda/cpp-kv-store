#pragma once

#include <chrono>
#include <cstddef>
#include <stop_token>
#include <thread>

#include "kvstore/storage/store.hpp"

namespace kvstore::storage {

struct TtlSweeperConfig {
  // How often to run a sweep pass.
  std::chrono::milliseconds interval{100};
  // Bounded work per shard per pass (see Store::sweep_expired).
  std::size_t max_items_per_shard = 20;
};

// A single background maintenance thread that periodically calls
// Store::sweep_expired() to actively remove expired keys, complementing
// the storage engine's own lazy (on-access) expiration -- SPEC.md section
// 7: "Active expiration: a background mechanism periodically removes
// expired keys." Owns exactly one std::jthread; its destructor requests
// stop and joins, leaving no joinable thread behind
// (docs/CONCURRENCY.md).
class TtlSweeper {
 public:
  explicit TtlSweeper(Store& store, TtlSweeperConfig config = {});
  ~TtlSweeper();

  TtlSweeper(const TtlSweeper&) = delete;
  TtlSweeper& operator=(const TtlSweeper&) = delete;
  TtlSweeper(TtlSweeper&&) = delete;
  TtlSweeper& operator=(TtlSweeper&&) = delete;

 private:
  void run(const std::stop_token& token);

  Store& store_;
  TtlSweeperConfig config_;
  std::jthread thread_;
};

}  // namespace kvstore::storage
