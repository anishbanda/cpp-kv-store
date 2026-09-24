#include "kvstore/worker/worker_pool.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>

#include "kvstore/storage/store.hpp"
#include "kvstore/worker/bounded_queue.hpp"
#include "kvstore/worker/work_item.hpp"

namespace kvstore::worker {
namespace {

template <typename T>
std::optional<T> try_pop_with_retry(BoundedQueue<T>& queue, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (std::optional<T> item = queue.try_pop()) {
      return item;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return std::nullopt;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}

WorkItem make_set_item(RequestSequence sequence, std::string key, std::string value) {
  return WorkItem{.connection_id = 1,
                  .connection_generation = 1,
                  .sequence = sequence,
                  .command = protocol::Command{.type = protocol::CommandType::kSet,
                                               .key = std::move(key),
                                               .value = std::move(value),
                                               .seconds = {}}};
}

WorkItem make_get_item(RequestSequence sequence, std::string key) {
  return WorkItem{
      .connection_id = 1,
      .connection_generation = 1,
      .sequence = sequence,
      .command = protocol::Command{
          .type = protocol::CommandType::kGet, .key = std::move(key), .value = {}, .seconds = {}}};
}

TEST(WorkerPoolTest, ZeroWorkersThrows) {
  storage::Store store;
  BoundedQueue<WorkItem> work_queue(4);
  BoundedQueue<Completion> completion_queue(4);
  EXPECT_THROW(WorkerPool(0, store, work_queue, completion_queue), std::invalid_argument);
}

TEST(WorkerPoolTest, ProcessesOneItemAndProducesTheCorrectCompletion) {
  storage::Store store;
  BoundedQueue<WorkItem> work_queue(4);
  BoundedQueue<Completion> completion_queue(4);
  WorkerPool pool(2, store, work_queue, completion_queue);

  ASSERT_EQ(work_queue.push(make_set_item(0, "k", "v")), PushStatus::kOk);

  const std::optional<Completion> completion =
      try_pop_with_retry(completion_queue, std::chrono::seconds(2));
  ASSERT_TRUE(completion.has_value());
  EXPECT_EQ(completion->connection_id, 1);
  EXPECT_EQ(completion->sequence, 0u);
  EXPECT_EQ(completion->response, "+OK\r\n");
  EXPECT_EQ(store.get("k"), "v");
}

TEST(WorkerPoolTest, MultipleWorkersProcessManyItemsCorrectly) {
  storage::Store store;
  BoundedQueue<WorkItem> work_queue(16);
  BoundedQueue<Completion> completion_queue(256);
  WorkerPool pool(4, store, work_queue, completion_queue);

  constexpr int kCount = 200;
  std::thread producer([&] {
    for (int i = 0; i < kCount; ++i) {
      ASSERT_EQ(
          work_queue.push(make_set_item(static_cast<RequestSequence>(i), "key" + std::to_string(i),
                                        "value" + std::to_string(i))),
          PushStatus::kOk);
    }
  });

  std::unordered_set<RequestSequence> seen_sequences;
  for (int i = 0; i < kCount; ++i) {
    const std::optional<Completion> completion =
        try_pop_with_retry(completion_queue, std::chrono::seconds(5));
    ASSERT_TRUE(completion.has_value()) << "missing completion for iteration " << i;
    EXPECT_EQ(completion->response, "+OK\r\n");
    seen_sequences.insert(completion->sequence);
  }
  producer.join();

  EXPECT_EQ(seen_sequences.size(), static_cast<std::size_t>(kCount));
  for (int i = 0; i < kCount; ++i) {
    EXPECT_EQ(store.get("key" + std::to_string(i)), "value" + std::to_string(i));
  }
}

TEST(WorkerPoolTest, GetSeesValuesWrittenBySet) {
  storage::Store store;
  BoundedQueue<WorkItem> work_queue(4);
  BoundedQueue<Completion> completion_queue(4);
  WorkerPool pool(1, store, work_queue, completion_queue);  // one worker: strict order

  ASSERT_EQ(work_queue.push(make_set_item(0, "k", "hello")), PushStatus::kOk);
  ASSERT_TRUE(try_pop_with_retry(completion_queue, std::chrono::seconds(2)).has_value());

  ASSERT_EQ(work_queue.push(make_get_item(1, "k")), PushStatus::kOk);
  const std::optional<Completion> completion =
      try_pop_with_retry(completion_queue, std::chrono::seconds(2));
  ASSERT_TRUE(completion.has_value());
  EXPECT_EQ(completion->response, "$5\r\nhello\r\n");
}

TEST(WorkerPoolTest, DestructorDrainsAlreadyQueuedWorkBeforeStopping) {
  storage::Store store;
  BoundedQueue<WorkItem> work_queue(64);
  BoundedQueue<Completion> completion_queue(64);

  constexpr int kCount = 20;
  {
    WorkerPool pool(2, store, work_queue, completion_queue);
    for (int i = 0; i < kCount; ++i) {
      ASSERT_EQ(
          work_queue.push(make_set_item(static_cast<RequestSequence>(i), "key" + std::to_string(i),
                                        "value" + std::to_string(i))),
          PushStatus::kOk);
    }
    // Pool destructs here without the test draining completions first --
    // its destructor must close the work queue and let workers finish
    // everything already queued before their threads exit (see
    // docs/CONCURRENCY.md, "Shutdown Order").
  }

  int drained = 0;
  while (completion_queue.try_pop().has_value()) {
    ++drained;
  }
  EXPECT_EQ(drained, kCount);
  for (int i = 0; i < kCount; ++i) {
    EXPECT_EQ(store.get("key" + std::to_string(i)), "value" + std::to_string(i));
  }
}

}  // namespace
}  // namespace kvstore::worker
