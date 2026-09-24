#include "kvstore/storage/store.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
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

}  // namespace
}  // namespace kvstore::storage
