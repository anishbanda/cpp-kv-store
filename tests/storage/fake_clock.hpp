#pragma once

#include "kvstore/storage/clock.hpp"

namespace kvstore::storage::testing {

// Deterministic clock for TTL tests. Not thread-safe: only advance it from
// the thread that owns it, before or between concurrent operations, not
// concurrently with them.
class FakeClock final : public Clock {
 public:
  [[nodiscard]] TimePoint now() const override { return now_; }

  void advance(SteadyClock::duration delta) { now_ += delta; }

 private:
  TimePoint now_{};
};

}  // namespace kvstore::storage::testing
