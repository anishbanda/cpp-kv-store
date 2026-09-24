#include "kvstore/worker/worker_pool.hpp"

#include <stdexcept>
#include <utility>

#include "kvstore/command/dispatcher.hpp"

namespace kvstore::worker {

WorkerPool::WorkerPool(std::size_t worker_count, storage::Store& store,
                       BoundedQueue<WorkItem>& work_queue,
                       BoundedQueue<Completion>& completion_queue)
    : store_(store), work_queue_(work_queue), completion_queue_(completion_queue) {
  if (worker_count == 0) {
    throw std::invalid_argument("WorkerPool: worker_count must be greater than zero");
  }
  workers_.reserve(worker_count);
  for (std::size_t i = 0; i < worker_count; ++i) {
    workers_.emplace_back([this](const std::stop_token& token) { worker_main(token); });
  }
}

WorkerPool::~WorkerPool() {
  // Wakes any worker blocked in pop(); items already queued are still
  // drained normally (pop() returns kOk) before pop() finally reports
  // kClosed. std::jthread's own destructor (run below, for each element
  // of `workers_`, in reverse declaration order) additionally requests
  // stop and joins, so no joinable thread is ever left behind.
  work_queue_.close();
}

void WorkerPool::worker_main(const std::stop_token& token) {
  for (;;) {
    WorkItem item;
    const PopStatus pop_status = work_queue_.pop(item, token);
    if (pop_status != PopStatus::kOk) {
      return;
    }

    std::string response = command::dispatch(store_, item.command);
    Completion completion{.connection_id = item.connection_id,
                          .connection_generation = item.connection_generation,
                          .sequence = item.sequence,
                          .response = std::move(response)};

    const PushStatus push_status = completion_queue_.push(std::move(completion));
    if (push_status != PushStatus::kOk) {
      return;  // completion queue closed: the system is shutting down
    }
  }
}

}  // namespace kvstore::worker
