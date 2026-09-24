#include "kvstore/storage/store.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "fake_clock.hpp"

namespace kvstore::storage {
namespace {

using ::kvstore::storage::testing::FakeClock;

class StoreTest : public ::testing::Test {
 protected:
  FakeClock clock_;
  Store store_{StoreOptions{}, clock_};
};

// --- Normal behavior -------------------------------------------------------

TEST_F(StoreTest, GetMissingKeyReturnsNullopt) { EXPECT_EQ(store_.get("missing"), std::nullopt); }

TEST_F(StoreTest, SetThenGetReturnsValue) {
  store_.set("k", "v1");
  EXPECT_EQ(store_.get("k"), "v1");
}

TEST_F(StoreTest, SetOverwritesExistingValue) {
  store_.set("k", "v1");
  store_.set("k", "v2");
  EXPECT_EQ(store_.get("k"), "v2");
}

TEST_F(StoreTest, ExistsReflectsLiveKeys) {
  EXPECT_FALSE(store_.exists("k"));
  store_.set("k", "v1");
  EXPECT_TRUE(store_.exists("k"));
}

TEST_F(StoreTest, DelRemovesLiveKeyAndReturnsTrue) {
  store_.set("k", "v1");
  EXPECT_TRUE(store_.del("k"));
  EXPECT_FALSE(store_.exists("k"));
  EXPECT_EQ(store_.get("k"), std::nullopt);
}

// --- Missing keys ------------------------------------------------------------

TEST_F(StoreTest, DelMissingKeyReturnsFalse) { EXPECT_FALSE(store_.del("missing")); }

TEST_F(StoreTest, ExpireMissingKeyReturnsFalse) {
  EXPECT_FALSE(store_.expire("missing", std::chrono::seconds{10}));
}

TEST_F(StoreTest, TtlMissingKeyReportsMissing) {
  const TtlResult result = store_.ttl("missing");
  EXPECT_EQ(result.status, TtlResult::Status::kMissing);
  EXPECT_EQ(result.remaining, std::nullopt);
}

// --- Replacement clears TTL --------------------------------------------------

TEST_F(StoreTest, ReplacingAKeyClearsItsTtl) {
  store_.set("k", "v1");
  ASSERT_TRUE(store_.expire("k", std::chrono::seconds{10}));
  ASSERT_EQ(store_.ttl("k").status, TtlResult::Status::kHasTtl);

  store_.set("k", "v2");

  const TtlResult result = store_.ttl("k");
  EXPECT_EQ(result.status, TtlResult::Status::kNoTtl);
  EXPECT_EQ(result.remaining, std::nullopt);
  EXPECT_EQ(store_.get("k"), "v2");
}

// --- Immediate expiration -----------------------------------------------------

TEST_F(StoreTest, ExpireWithNonPositiveTtlExpiresImmediately) {
  store_.set("k", "v1");
  EXPECT_TRUE(store_.expire("k", std::chrono::seconds{0}));

  EXPECT_EQ(store_.get("k"), std::nullopt);
  EXPECT_FALSE(store_.exists("k"));
  EXPECT_EQ(store_.ttl("k").status, TtlResult::Status::kMissing);
}

TEST_F(StoreTest, KeyExpiresOnceClockReachesDeadline) {
  store_.set("k", "v1");
  ASSERT_TRUE(store_.expire("k", std::chrono::seconds{5}));

  clock_.advance(std::chrono::seconds{4});
  EXPECT_EQ(store_.get("k"), "v1");

  clock_.advance(std::chrono::seconds{1});
  EXPECT_EQ(store_.get("k"), std::nullopt);
  EXPECT_FALSE(store_.exists("k"));
  EXPECT_FALSE(store_.del("k"));
}

TEST_F(StoreTest, ExpireOnAlreadyExpiredKeyReturnsFalse) {
  store_.set("k", "v1");
  ASSERT_TRUE(store_.expire("k", std::chrono::seconds{1}));
  clock_.advance(std::chrono::seconds{1});

  EXPECT_FALSE(store_.expire("k", std::chrono::seconds{10}));
  EXPECT_EQ(store_.get("k"), std::nullopt);
}

// --- TTL return values --------------------------------------------------------

TEST_F(StoreTest, TtlWithNoExpirationReportsNoTtl) {
  store_.set("k", "v1");
  const TtlResult result = store_.ttl("k");
  EXPECT_EQ(result.status, TtlResult::Status::kNoTtl);
  EXPECT_EQ(result.remaining, std::nullopt);
}

TEST_F(StoreTest, TtlRoundsDownToWholeSecondsRemaining) {
  store_.set("k", "v1");
  ASSERT_TRUE(store_.expire("k", std::chrono::seconds{10}));

  clock_.advance(std::chrono::milliseconds{4900});

  const TtlResult result = store_.ttl("k");
  EXPECT_EQ(result.status, TtlResult::Status::kHasTtl);
  ASSERT_TRUE(result.remaining.has_value());
  EXPECT_EQ(result.remaining->count(), 5);
}

// --- Active expiration (Milestone 6) ------------------------------------------

TEST_F(StoreTest, SweepExpiredRemovesNothingBeforeTheDeadline) {
  store_.set("k", "v1");
  ASSERT_TRUE(store_.expire("k", std::chrono::seconds{10}));

  EXPECT_EQ(store_.sweep_expired(/*max_items_per_shard=*/16), 0u);
  EXPECT_EQ(store_.active_expiration_count(), 0u);
  EXPECT_EQ(store_.get("k"), "v1");
}

TEST_F(StoreTest, SweepExpiredRemovesAKeyPastItsDeadline) {
  store_.set("k", "v1");
  ASSERT_TRUE(store_.expire("k", std::chrono::seconds{5}));
  clock_.advance(std::chrono::seconds{5});

  EXPECT_EQ(store_.sweep_expired(/*max_items_per_shard=*/16), 1u);
  EXPECT_EQ(store_.active_expiration_count(), 1u);

  // Removed without ever being accessed via get()/exists() -- proves
  // active, not lazy, expiration did this.
}

TEST_F(StoreTest, SweepExpiredIgnoresAKeyThatWasReplacedBeforeTheSweep) {
  store_.set("k", "v1");
  ASSERT_TRUE(store_.expire("k", std::chrono::seconds{5}));
  clock_.advance(std::chrono::seconds{5});

  // Replacing the key bumps its generation, invalidating the scheduled
  // expiration candidate (docs/CONCURRENCY.md, "TTL Races").
  store_.set("k", "v2");

  EXPECT_EQ(store_.sweep_expired(/*max_items_per_shard=*/16), 0u);
  EXPECT_EQ(store_.active_expiration_count(), 0u);
  EXPECT_EQ(store_.get("k"), "v2");
}

TEST_F(StoreTest, SweepExpiredIgnoresAKeyThatWasReExpiredWithALaterDeadline) {
  store_.set("k", "v1");
  ASSERT_TRUE(store_.expire("k", std::chrono::seconds{1}));    // stale candidate, due first
  ASSERT_TRUE(store_.expire("k", std::chrono::seconds{100}));  // supersedes it

  clock_.advance(std::chrono::seconds{1});  // the stale candidate's deadline has passed

  EXPECT_EQ(store_.sweep_expired(/*max_items_per_shard=*/16), 0u);
  EXPECT_EQ(store_.get("k"), "v1");
  EXPECT_EQ(store_.ttl("k").status, TtlResult::Status::kHasTtl);
}

TEST_F(StoreTest, SweepExpiredIgnoresAKeyThatWasDeletedBeforeTheSweep) {
  store_.set("k", "v1");
  ASSERT_TRUE(store_.expire("k", std::chrono::seconds{5}));
  clock_.advance(std::chrono::seconds{5});

  ASSERT_FALSE(store_.del("k"));  // already lazily-expired from del()'s own point of view

  EXPECT_EQ(store_.sweep_expired(/*max_items_per_shard=*/16), 0u);
  EXPECT_EQ(store_.active_expiration_count(), 0u);
}

TEST_F(StoreTest, SweepExpiredHonorsThePerShardBatchLimit) {
  // A single-shard store puts every key's expiration candidate in the same
  // heap, making the per-call cap directly observable.
  FakeClock clock;
  Store store(StoreOptions{.shard_count = 1}, clock);

  constexpr int kKeys = 10;
  for (int i = 0; i < kKeys; ++i) {
    const std::string key = "k" + std::to_string(i);
    store.set(key, "v");
    ASSERT_TRUE(store.expire(key, std::chrono::seconds{1}));
  }
  clock.advance(std::chrono::seconds{1});

  EXPECT_EQ(store.sweep_expired(/*max_items_per_shard=*/4), 4u);
  EXPECT_EQ(store.sweep_expired(/*max_items_per_shard=*/4), 4u);
  EXPECT_EQ(store.sweep_expired(/*max_items_per_shard=*/4), 2u);
  EXPECT_EQ(store.sweep_expired(/*max_items_per_shard=*/4), 0u);
  EXPECT_EQ(store.active_expiration_count(), static_cast<std::uint64_t>(kKeys));
}

// --- Size / configuration boundaries -----------------------------------------

TEST(StoreConfigTest, ZeroShardCountThrows) {
  EXPECT_THROW(Store store(StoreOptions{.shard_count = 0}), std::invalid_argument);
}

TEST(StoreConfigTest, DefaultShardCountIsThirtyTwo) {
  Store store;
  EXPECT_EQ(store.shard_count(), 32u);
}

TEST(StoreConfigTest, NonPowerOfTwoShardCountRoundsUp) {
  Store store(StoreOptions{.shard_count = 5});
  EXPECT_EQ(store.shard_count(), 8u);
}

TEST(StoreConfigTest, SingleShardStoreWorks) {
  Store store(StoreOptions{.shard_count = 1});
  store.set("a", "1");
  store.set("b", "2");
  EXPECT_EQ(store.get("a"), "1");
  EXPECT_EQ(store.get("b"), "2");
}

TEST(StoreConfigTest, EmptyKeyAndEmptyValueRoundTrip) {
  Store store;
  store.set("", "");
  EXPECT_TRUE(store.exists(""));
  EXPECT_EQ(store.get(""), "");
}

TEST(StoreConfigTest, LargeValueRoundTrips) {
  Store store;
  const std::string large_value(2 * 1024 * 1024, 'x');  // 2 MiB, larger than the protocol cap.
  store.set("k", large_value);
  const std::optional<std::string> result = store.get("k");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->size(), large_value.size());
  EXPECT_EQ(*result, large_value);
}

// --- Concurrent access ---------------------------------------------------------
//
// These exercise the scenarios documented in docs/CONCURRENCY.md #8 that are
// in scope for Milestone 1's synchronous storage engine. The 1,000-connection
// and randomized-invariant stress suites are Milestone 7 work.

TEST(StoreConcurrencyTest, ManyReadersOfSameKeySeeAValidValue) {
  Store store;
  const std::unordered_set<std::string> valid_values{"v0", "v1", "v2"};
  store.set("k", "v0");

  constexpr int kReaders = 4;
  constexpr int kIterations = 300;
  std::atomic<bool> stop{false};
  std::vector<std::thread> readers;
  std::atomic<bool> saw_invalid{false};

  for (int i = 0; i < kReaders; ++i) {
    readers.emplace_back([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        const std::optional<std::string> value = store.get("k");
        if (!value.has_value() || !valid_values.contains(*value)) {
          saw_invalid.store(true, std::memory_order_relaxed);
        }
      }
    });
  }

  for (int i = 1; i <= kIterations; ++i) {
    store.set("k", "v" + std::to_string(i % 3));
  }
  stop.store(true, std::memory_order_relaxed);
  for (std::thread& t : readers) {
    t.join();
  }

  EXPECT_FALSE(saw_invalid.load());
}

TEST(StoreConcurrencyTest, ReadersAndWriterOfOneKeyDoNotCrash) {
  Store store;
  store.set("k", "initial");

  constexpr int kIterations = 1000;
  std::atomic<bool> stop{false};

  std::thread writer([&] {
    for (int i = 0; i < kIterations; ++i) {
      store.set("k", "v" + std::to_string(i));
    }
    stop.store(true, std::memory_order_relaxed);
  });

  std::thread reader([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      [[maybe_unused]] const bool exists = store.exists("k");
      [[maybe_unused]] const std::optional<std::string> value = store.get("k");
    }
  });

  writer.join();
  reader.join();

  EXPECT_TRUE(store.exists("k"));
}

TEST(StoreConcurrencyTest, IndependentWritesAcrossShardsAllPersist) {
  Store store(StoreOptions{.shard_count = 16});

  constexpr int kKeys = 64;
  std::vector<std::thread> writers;
  for (int i = 0; i < kKeys; ++i) {
    writers.emplace_back(
        [&store, i] { store.set("key" + std::to_string(i), "value" + std::to_string(i)); });
  }
  for (std::thread& t : writers) {
    t.join();
  }

  for (int i = 0; i < kKeys; ++i) {
    EXPECT_EQ(store.get("key" + std::to_string(i)), "value" + std::to_string(i));
  }
}

TEST(StoreConcurrencyTest, DeletionRacesWithGetAndTtlWithoutCrashing) {
  Store store;

  constexpr int kIterations = 1000;
  std::atomic<bool> stop{false};

  std::thread writer([&] {
    for (int i = 0; i < kIterations; ++i) {
      store.set("k", "v" + std::to_string(i));
      [[maybe_unused]] const bool set_ttl = store.expire("k", std::chrono::seconds{60});
    }
    stop.store(true, std::memory_order_relaxed);
  });

  std::thread deleter([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      [[maybe_unused]] const bool deleted = store.del("k");
    }
  });

  std::thread reader([&] {
    while (!stop.load(std::memory_order_relaxed)) {
      [[maybe_unused]] const std::optional<std::string> value = store.get("k");
      [[maybe_unused]] const TtlResult ttl_result = store.ttl("k");
    }
  });

  writer.join();
  deleter.join();
  reader.join();

  // No crash and the store remains usable is the primary assertion here;
  // this also confirms the store did not end up in a stuck/locked state.
  store.set("k", "final");
  EXPECT_EQ(store.get("k"), "final");
}

// Milestone 6: a background sweep thread races real writer threads that
// repeatedly expire and then replace their own keys. The clock stays
// frozen throughout (safe to read concurrently; only advance() would
// race), so every "expire" below is already due the instant it is set.
// Each key's very last write clears its TTL, so once all writers finish,
// the sweeper must never have deleted any of them -- proving the
// generation check holds under real concurrency, not just in the
// single-threaded tests above.
TEST(StoreConcurrencyTest, ActiveSweepDoesNotRemoveKeysReplacedDuringARace) {
  FakeClock clock;
  Store store(StoreOptions{.shard_count = 8}, clock);

  constexpr int kWriterThreads = 4;
  constexpr int kKeysPerThread = 25;
  constexpr int kIterationsPerKey = 20;

  std::atomic<bool> stop_sweeping{false};
  std::thread sweeper([&] {
    while (!stop_sweeping.load(std::memory_order_relaxed)) {
      (void)store.sweep_expired(16);
    }
  });

  std::vector<std::thread> writers;
  for (int t = 0; t < kWriterThreads; ++t) {
    writers.emplace_back([&store, t] {
      for (int k = 0; k < kKeysPerThread; ++k) {
        const std::string key = "t" + std::to_string(t) + "-" + std::to_string(k);
        for (int iter = 0; iter < kIterationsPerKey; ++iter) {
          store.set(key, "temp");
          [[maybe_unused]] const bool expired = store.expire(key, std::chrono::seconds{0});
        }
        store.set(key, "final");  // clears the TTL; nothing touches this key again
      }
    });
  }
  for (std::thread& writer : writers) {
    writer.join();
  }
  stop_sweeping.store(true, std::memory_order_relaxed);
  sweeper.join();

  for (int t = 0; t < kWriterThreads; ++t) {
    for (int k = 0; k < kKeysPerThread; ++k) {
      const std::string key = "t" + std::to_string(t) + "-" + std::to_string(k);
      EXPECT_EQ(store.get(key), "final") << "key=" << key;
    }
  }
}

}  // namespace
}  // namespace kvstore::storage
