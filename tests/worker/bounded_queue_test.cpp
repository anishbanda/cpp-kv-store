#include "kvstore/worker/bounded_queue.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <stop_token>
#include <thread>

namespace kvstore::worker {
namespace {

TEST(BoundedQueueTest, ZeroCapacityThrows) {
  EXPECT_THROW(BoundedQueue<int>(0), std::invalid_argument);
}

TEST(BoundedQueueTest, StartsEmptyAndOpen) {
  const BoundedQueue<int> queue(4);
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_EQ(queue.capacity(), 4u);
  EXPECT_FALSE(queue.closed());
}

TEST(BoundedQueueTest, PushThenPopRoundTrips) {
  BoundedQueue<int> queue(4);
  ASSERT_EQ(queue.push(42), PushStatus::kOk);

  int value = 0;
  const std::stop_source source;
  ASSERT_EQ(queue.pop(value, source.get_token()), PopStatus::kOk);
  EXPECT_EQ(value, 42);
}

TEST(BoundedQueueTest, PreservesFifoOrder) {
  BoundedQueue<int> queue(4);
  ASSERT_EQ(queue.push(1), PushStatus::kOk);
  ASSERT_EQ(queue.push(2), PushStatus::kOk);
  ASSERT_EQ(queue.push(3), PushStatus::kOk);

  const std::stop_source source;
  int value = 0;
  ASSERT_EQ(queue.pop(value, source.get_token()), PopStatus::kOk);
  EXPECT_EQ(value, 1);
  ASSERT_EQ(queue.pop(value, source.get_token()), PopStatus::kOk);
  EXPECT_EQ(value, 2);
  ASSERT_EQ(queue.pop(value, source.get_token()), PopStatus::kOk);
  EXPECT_EQ(value, 3);
}

TEST(BoundedQueueTest, TryPushFailsWithoutBlockingWhenFull) {
  BoundedQueue<int> queue(2);
  int a = 1;
  int b = 2;
  int c = 3;
  ASSERT_EQ(queue.try_push(a), PushStatus::kOk);
  ASSERT_EQ(queue.try_push(b), PushStatus::kOk);

  EXPECT_EQ(queue.try_push(c), PushStatus::kFull);
  EXPECT_EQ(c, 3);  // untouched on failure
}

TEST(BoundedQueueTest, TryPopReturnsNulloptWhenEmpty) {
  BoundedQueue<int> queue(2);
  EXPECT_EQ(queue.try_pop(), std::nullopt);
}

TEST(BoundedQueueTest, TryPopReturnsAvailableItemWithoutBlocking) {
  BoundedQueue<int> queue(2);
  ASSERT_EQ(queue.push(7), PushStatus::kOk);
  const std::optional<int> value = queue.try_pop();
  ASSERT_TRUE(value.has_value());
  EXPECT_EQ(*value, 7);
}

// --- close(): queue closure ---------------------------------------------

TEST(BoundedQueueTest, PushAfterCloseIsRejected) {
  BoundedQueue<int> queue(2);
  queue.close();
  EXPECT_EQ(queue.push(1), PushStatus::kClosed);
}

TEST(BoundedQueueTest, TryPushAfterCloseIsRejected) {
  BoundedQueue<int> queue(2);
  queue.close();
  int value = 1;
  EXPECT_EQ(queue.try_push(value), PushStatus::kClosed);
}

TEST(BoundedQueueTest, PopDrainsRemainingItemsAfterCloseThenReportsClosed) {
  BoundedQueue<int> queue(4);
  ASSERT_EQ(queue.push(1), PushStatus::kOk);
  ASSERT_EQ(queue.push(2), PushStatus::kOk);
  queue.close();

  const std::stop_source source;
  int value = 0;
  EXPECT_EQ(queue.pop(value, source.get_token()), PopStatus::kOk);
  EXPECT_EQ(value, 1);
  EXPECT_EQ(queue.pop(value, source.get_token()), PopStatus::kOk);
  EXPECT_EQ(value, 2);
  EXPECT_EQ(queue.pop(value, source.get_token()), PopStatus::kClosed);
}

TEST(BoundedQueueTest, CloseWakesABlockedPop) {
  BoundedQueue<int> queue(2);
  std::atomic<bool> woke{false};
  const std::stop_source source;

  std::thread waiter([&] {
    int value = 0;
    const PopStatus status = queue.pop(value, source.get_token());
    EXPECT_EQ(status, PopStatus::kClosed);
    woke.store(true, std::memory_order_release);
  });

  // Give the waiter a chance to actually be blocked in pop() before closing.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  queue.close();
  waiter.join();
  EXPECT_TRUE(woke.load());
}

TEST(BoundedQueueTest, CloseWakesABlockedPush) {
  BoundedQueue<int> queue(1);
  ASSERT_EQ(queue.push(1), PushStatus::kOk);  // fill capacity

  std::atomic<bool> woke{false};
  std::thread pusher([&] {
    const PushStatus status = queue.push(2);
    EXPECT_EQ(status, PushStatus::kClosed);
    woke.store(true, std::memory_order_release);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  queue.close();
  pusher.join();
  EXPECT_TRUE(woke.load());
}

TEST(BoundedQueueTest, CloseIsIdempotent) {
  BoundedQueue<int> queue(2);
  queue.close();
  queue.close();
  EXPECT_TRUE(queue.closed());
}

// --- stop_token interruption ---------------------------------------------

TEST(BoundedQueueTest, PopReturnsStoppedWhenStopRequestedWhileBlocked) {
  BoundedQueue<int> queue(2);  // left empty and open
  std::stop_source source;
  std::atomic<bool> stopped{false};

  std::thread waiter([&] {
    int value = 0;
    const PopStatus status = queue.pop(value, source.get_token());
    EXPECT_EQ(status, PopStatus::kStopped);
    stopped.store(true, std::memory_order_release);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  source.request_stop();
  waiter.join();
  EXPECT_TRUE(stopped.load());
  EXPECT_FALSE(queue.closed());  // stopping a waiter is not the same as closing the queue
}

TEST(BoundedQueueTest, PopPrefersAvailableItemOverAlreadyRequestedStop) {
  BoundedQueue<int> queue(2);
  ASSERT_EQ(queue.push(9), PushStatus::kOk);

  std::stop_source source;
  source.request_stop();  // requested before pop() is even called

  int value = 0;
  EXPECT_EQ(queue.pop(value, source.get_token()), PopStatus::kOk);
  EXPECT_EQ(value, 9);
}

}  // namespace
}  // namespace kvstore::worker
