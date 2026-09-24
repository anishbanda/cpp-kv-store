#include "kvstore/storage/ttl_sweeper.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace kvstore::storage {
namespace {

// Uses the real system clock and short real TTLs/intervals: TtlSweeper
// runs on its own background thread on a wall-clock timer, so unlike the
// rest of storage's tests it cannot be driven by a FakeClock (see
// FakeClock's own "not thread-safe" note -- the sweeper thread would be
// reading it concurrently with any advance() call from the test thread).

TEST(TtlSweeperTest, ActivelyRemovesAnExpiredKeyWithoutBeingAccessed) {
  Store store;
  store.set("k", "v");
  ASSERT_TRUE(store.expire("k", std::chrono::seconds{0}));  // already due

  TtlSweeperConfig config;
  config.interval = std::chrono::milliseconds{10};
  config.max_items_per_shard = 100;
  TtlSweeper sweeper(store, config);

  // Poll active_expiration_count(), never get()/exists() -- either of
  // those would also lazily expire the key on access, which would mask
  // whether active expiration actually did the work.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (store.active_expiration_count() == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_EQ(store.active_expiration_count(), 1u);
}

TEST(TtlSweeperTest, NeverRemovesALiveKey) {
  Store store;
  store.set("k", "v");
  ASSERT_TRUE(store.expire("k", std::chrono::seconds{60}));

  TtlSweeperConfig config;
  config.interval = std::chrono::milliseconds{10};
  {
    TtlSweeper sweeper(store, config);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }  // stops and joins here

  EXPECT_EQ(store.active_expiration_count(), 0u);
  EXPECT_EQ(store.get("k"), "v");
}

TEST(TtlSweeperTest, DestructorStopsAndJoinsPromptlyEvenMidInterval) {
  Store store;
  TtlSweeperConfig config;
  config.interval = std::chrono::seconds{30};  // much longer than the destructor should ever wait

  const auto start = std::chrono::steady_clock::now();
  { TtlSweeper sweeper(store, config); }
  const auto elapsed = std::chrono::steady_clock::now() - start;

  EXPECT_LT(elapsed, std::chrono::seconds(1));
}

}  // namespace
}  // namespace kvstore::storage
