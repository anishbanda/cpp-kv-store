// Milestone 7: "Add randomized concurrent operation test with a reference
// model" (TASKS.md) / docs/CONCURRENCY.md #8, "high-volume randomized
// operations with invariant checking".
//
// Several threads each run a long randomized sequence of SET/GET/DEL/
// EXISTS/EXPIRE/TTL commands against the same shared storage::Store,
// running fully concurrently -- no lock serializes them, so shard
// mutexes, hash distribution, and the atomic expiration counter all see
// genuine multi-thread contention. Each thread owns a disjoint slice of
// the keyspace and checks every result against its own thread-local
// reference model immediately: since no other thread ever touches those
// keys, the expected value is unambiguous without needing a global lock
// or a full linearizability checker. The clock is frozen throughout, so
// EXPIRE/TTL state transitions are exercised without needing to reason
// about real elapsed time inside the race.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../storage/fake_clock.hpp"
#include "kvstore/storage/store.hpp"

namespace kvstore::reliability {
namespace {

using storage::Store;
using storage::TtlResult;
using storage::testing::FakeClock;

struct ModelEntry {
  std::string value;
  bool has_ttl = false;
};

enum class Op : std::uint8_t { kSet, kGet, kDel, kExists, kExpire, kTtl };

TEST(ReliabilityTest, RandomizedConcurrentOperationsMatchAReferenceModel) {
  FakeClock clock;  // frozen: no key ever expires mid-test on its own
  Store store(storage::StoreOptions{.shard_count = 8}, clock);

  constexpr int kThreads = 8;
  constexpr int kOpsPerThread = 3000;
  constexpr int kKeysPerThread = 12;  // small on purpose: repeated hits on the same few keys

  std::atomic<int> mismatch_count{0};

  auto worker = [&](int thread_index) {
    std::mt19937 rng(static_cast<unsigned>(thread_index + 1));
    std::uniform_int_distribution<int> key_dist(0, kKeysPerThread - 1);
    std::uniform_int_distribution<int> op_dist(0, 5);
    std::uniform_int_distribution<int> value_dist(0, 1'000'000);

    // Thread-local: only this thread ever touches these keys, so the
    // model needs no synchronization of its own.
    std::unordered_map<std::string, ModelEntry> model;

    for (int i = 0; i < kOpsPerThread; ++i) {
      const std::string key =
          "t" + std::to_string(thread_index) + "-" + std::to_string(key_dist(rng));
      const Op op = static_cast<Op>(op_dist(rng));
      const auto model_it = model.find(key);
      const bool expected_present = model_it != model.end();

      switch (op) {
        case Op::kSet: {
          const std::string value = "v" + std::to_string(value_dist(rng));
          store.set(key, value);
          model[key] = ModelEntry{.value = value, .has_ttl = false};
          break;
        }
        case Op::kGet: {
          const std::optional<std::string> actual = store.get(key);
          const std::optional<std::string> expected =
              expected_present ? std::optional<std::string>(model_it->second.value) : std::nullopt;
          if (actual != expected) {
            mismatch_count.fetch_add(1, std::memory_order_relaxed);
          }
          break;
        }
        case Op::kDel: {
          const bool actual = store.del(key);
          if (actual != expected_present) {
            mismatch_count.fetch_add(1, std::memory_order_relaxed);
          }
          model.erase(key);
          break;
        }
        case Op::kExists: {
          const bool actual = store.exists(key);
          if (actual != expected_present) {
            mismatch_count.fetch_add(1, std::memory_order_relaxed);
          }
          break;
        }
        case Op::kExpire: {
          const bool actual = store.expire(key, std::chrono::seconds{30});
          if (actual != expected_present) {
            mismatch_count.fetch_add(1, std::memory_order_relaxed);
          }
          if (expected_present) {
            model[key].has_ttl = true;
          }
          break;
        }
        case Op::kTtl: {
          const TtlResult actual = store.ttl(key);
          TtlResult::Status expected_status = TtlResult::Status::kMissing;
          if (expected_present) {
            expected_status =
                model_it->second.has_ttl ? TtlResult::Status::kHasTtl : TtlResult::Status::kNoTtl;
          }
          if (actual.status != expected_status) {
            mismatch_count.fetch_add(1, std::memory_order_relaxed);
          }
          break;
        }
      }
    }
  };

  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back(worker, t);
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(mismatch_count.load(), 0) << "operation(s) diverged from the reference model";
}

}  // namespace
}  // namespace kvstore::reliability
