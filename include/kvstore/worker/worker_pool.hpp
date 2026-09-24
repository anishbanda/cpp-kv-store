#pragma once

#include <cstddef>
#include <stop_token>
#include <thread>
#include <vector>

#include "kvstore/storage/store.hpp"
#include "kvstore/worker/bounded_queue.hpp"
#include "kvstore/worker/work_item.hpp"

namespace kvstore::worker {

// Owns a fixed number of worker threads that pull WorkItems from
// `work_queue`, dispatch them to `store` (kvstore::command::dispatch),
// and push the resulting Completions onto `completion_queue`. Workers
// never touch client sockets, connection objects, or epoll (see
// ARCHITECTURE.md, "Worker pool").
//
// Does not own the queues or the store: they are shared with the caller
// (typically also shared with an EventLoop), which must outlive this
// WorkerPool. This keeps networking and command execution decoupled --
// neither depends on the other's type.
//
// On destruction: closes `work_queue` (so a worker blocked in pop() wakes
// and any already-queued items are still drained -- see
// docs/CONCURRENCY.md, "Shutdown Order"), then each std::jthread's own
// destructor requests stop and joins, leaving no joinable threads behind.
class WorkerPool {
 public:
  WorkerPool(std::size_t worker_count, storage::Store& store, BoundedQueue<WorkItem>& work_queue,
             BoundedQueue<Completion>& completion_queue);
  ~WorkerPool();

  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;
  WorkerPool(WorkerPool&&) = delete;
  WorkerPool& operator=(WorkerPool&&) = delete;

 private:
  void worker_main(const std::stop_token& token);

  storage::Store& store_;
  BoundedQueue<WorkItem>& work_queue_;
  BoundedQueue<Completion>& completion_queue_;
  std::vector<std::jthread> workers_;
};

}  // namespace kvstore::worker
