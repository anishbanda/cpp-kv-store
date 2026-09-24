#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace kvstore::net {

// A byte buffer capped at a fixed capacity. append() rejects writes that
// would exceed the cap instead of growing unbounded -- see SPEC.md
// (per-connection input/output buffer caps, section 5) and ARCHITECTURE.md
// (section 4, Backpressure). Not thread-safe; owned by whichever connection
// holds it (the event-loop thread -- see docs/CONCURRENCY.md).
class BoundedBuffer {
 public:
  explicit BoundedBuffer(std::size_t capacity);

  // Appends `data` if it fits within the remaining capacity. Returns false,
  // leaving the buffer unchanged, if it would not -- callers should treat
  // that as backpressure / a resource limit, not a crash.
  [[nodiscard]] bool append(std::string_view data);

  // Removes the first `count` bytes. `count` beyond the current size
  // clears the buffer rather than underflowing.
  void consume(std::size_t count);

  [[nodiscard]] std::string_view view() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] bool empty() const noexcept { return data_.empty(); }

 private:
  std::string data_;
  std::size_t capacity_;
};

}  // namespace kvstore::net
