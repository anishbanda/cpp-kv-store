#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "kvstore/storage/clock.hpp"

namespace kvstore::storage::detail {

// Transparent hash so unordered_map lookups can take a std::string_view key
// without constructing a temporary std::string. Paired with std::equal_to<>,
// which is transparent, this enables heterogeneous find() (C++20).
struct TransparentStringHash {
  using is_transparent = void;

  [[nodiscard]] std::size_t operator()(std::string_view key) const noexcept {
    return std::hash<std::string_view>{}(key);
  }
};

// A stored value plus its optional expiration metadata.
//
// `generation` increases on every set() and expire() call. A later active
// expiration sweep (Milestone 6) can capture a (deadline, generation) pair
// and must delete the entry only if both still match at delete time --
// otherwise the key was replaced or re-expired since the sweep observed it,
// and deleting it would drop a newer value. See docs/CONCURRENCY.md #6.
struct Entry {
  std::string value;
  std::optional<TimePoint> deadline;
  std::uint64_t generation = 0;
};

// One shard: an independent lock plus the key/value map it guards. Reads
// take a shared lock; writes, deletes, and lazy-expiration cleanup take an
// exclusive lock. Never hold `mutex` while blocking on a queue, doing I/O,
// logging a payload, or sleeping.
struct Shard {
  mutable std::shared_mutex mutex;
  std::unordered_map<std::string, Entry, TransparentStringHash, std::equal_to<>> entries;
  std::uint64_t next_generation = 0;
};

[[nodiscard]] inline bool is_expired(const Entry& entry, TimePoint now) noexcept {
  return entry.deadline.has_value() && *entry.deadline <= now;
}

}  // namespace kvstore::storage::detail
