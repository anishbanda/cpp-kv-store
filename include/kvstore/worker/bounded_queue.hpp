#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <utility>

namespace kvstore::worker {

enum class PushStatus { kOk, kFull, kClosed };
enum class PopStatus { kOk, kClosed, kStopped };

// A fixed-capacity, thread-safe FIFO queue with close() support so blocked
// producers and consumers wake during shutdown (docs/CONCURRENCY.md,
// "Queues"). The event-loop thread must never block, so it uses the
// non-blocking try_push()/try_pop(); worker threads use the blocking
// push()/pop() overloads, the latter interruptible via a std::stop_token
// for std::jthread-driven shutdown.
template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
    if (capacity_ == 0) {
      throw std::invalid_argument("BoundedQueue: capacity must be greater than zero");
    }
  }

  BoundedQueue(const BoundedQueue&) = delete;
  BoundedQueue& operator=(const BoundedQueue&) = delete;
  BoundedQueue(BoundedQueue&&) = delete;
  BoundedQueue& operator=(BoundedQueue&&) = delete;

  using NotifyCallback = std::function<void()>;

  // Registers a callback invoked (outside any internal lock, after
  // notify_one()) once per successful push()/try_push() -- e.g. so a
  // consumer blocked in an unrelated wait (such as another thread's
  // epoll_wait) can be woken. Intended to be set once, before any
  // concurrent push()/try_push() begins; reading it during a push is
  // synchronized against a concurrent set_on_push(), but concurrent
  // set_on_push() calls with each other are not themselves ordered.
  void set_on_push(NotifyCallback callback) {
    std::lock_guard lock(mutex_);
    on_push_ = std::move(callback);
  }

  // Blocks while full and open. Returns kOk once `item` has been moved
  // into the queue, or kClosed if the queue is already, or becomes,
  // closed first (`item` is left unspecified-but-valid in that case, per
  // moved-from rules).
  [[nodiscard]] PushStatus push(T item) {
    NotifyCallback callback;
    {
      std::unique_lock lock(mutex_);
      not_full_.wait(lock, [this] { return closed_ || items_.size() < capacity_; });
      if (closed_) {
        return PushStatus::kClosed;
      }
      items_.push_back(std::move(item));
      callback = on_push_;
    }
    not_empty_.notify_one();
    if (callback) {
      callback();
    }
    return PushStatus::kOk;
  }

  // Never blocks. Returns kOk and moves from `item` if there was room,
  // kFull if the queue is open but at capacity (`item` is untouched), or
  // kClosed if the queue is closed (`item` is untouched).
  [[nodiscard]] PushStatus try_push(T& item) {
    NotifyCallback callback;
    {
      std::lock_guard lock(mutex_);
      if (closed_) {
        return PushStatus::kClosed;
      }
      if (items_.size() >= capacity_) {
        return PushStatus::kFull;
      }
      items_.push_back(std::move(item));
      callback = on_push_;
    }
    not_empty_.notify_one();
    if (callback) {
      callback();
    }
    return PushStatus::kOk;
  }

  // Blocks while empty and open, waking early if `token` requests stop.
  // Returns kOk with `out` populated, kClosed if the queue is closed and
  // now drained, or kStopped if `token` fired before either.
  [[nodiscard]] PopStatus pop(T& out, const std::stop_token& token) {
    std::unique_lock lock(mutex_);
    const bool predicate_satisfied =
        not_empty_.wait(lock, token, [this] { return closed_ || !items_.empty(); });
    if (!predicate_satisfied) {
      return PopStatus::kStopped;
    }
    if (items_.empty()) {
      return PopStatus::kClosed;
    }
    out = std::move(items_.front());
    items_.pop_front();
    lock.unlock();
    not_full_.notify_one();
    return PopStatus::kOk;
  }

  // Never blocks. Returns the front item if one was available, or
  // std::nullopt if the queue is currently empty (whether or not closed).
  [[nodiscard]] std::optional<T> try_pop() {
    std::unique_lock lock(mutex_);
    if (items_.empty()) {
      return std::nullopt;
    }
    T out = std::move(items_.front());
    items_.pop_front();
    lock.unlock();
    not_full_.notify_one();
    return out;
  }

  // Marks the queue closed: push()/try_push() reject from now on; blocked
  // and future pop()/try_pop() calls keep draining remaining items, then
  // report closed once empty. Wakes every blocked waiter. Idempotent.
  void close() {
    {
      std::lock_guard lock(mutex_);
      closed_ = true;
    }
    not_empty_.notify_all();
    not_full_.notify_all();
  }

  [[nodiscard]] bool closed() const noexcept {
    std::lock_guard lock(mutex_);
    return closed_;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    std::lock_guard lock(mutex_);
    return items_.size();
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

 private:
  mutable std::mutex mutex_;
  std::condition_variable_any not_full_;
  std::condition_variable_any not_empty_;
  std::deque<T> items_;
  std::size_t capacity_;
  bool closed_ = false;
  NotifyCallback on_push_;
};

}  // namespace kvstore::worker
