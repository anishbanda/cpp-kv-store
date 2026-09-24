#include "kvstore/storage/clock.hpp"

namespace kvstore::storage {

TimePoint SystemClock::now() const { return SteadyClock::now(); }

const SystemClock& SystemClock::instance() {
  static const SystemClock singleton;
  return singleton;
}

}  // namespace kvstore::storage
