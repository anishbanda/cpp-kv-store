#pragma once

#include <chrono>

namespace kvstore::storage {

using SteadyClock = std::chrono::steady_clock;
using TimePoint = SteadyClock::time_point;

// Injectable time source. Storage code reads time only through this
// interface so TTL behavior can be tested deterministically, without real
// sleeps, and so wall-clock changes never affect expiration.
class Clock {
 public:
  virtual ~Clock() = default;

  [[nodiscard]] virtual TimePoint now() const = 0;
};

// Production clock backed by std::chrono::steady_clock.
class SystemClock final : public Clock {
 public:
  [[nodiscard]] TimePoint now() const override;

  // Stateless, so a single shared instance is safe to hand out as the
  // default clock.
  [[nodiscard]] static const SystemClock& instance();
};

}  // namespace kvstore::storage
