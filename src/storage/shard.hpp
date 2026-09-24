#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

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

// A candidate for active expiration: captures a key's deadline and
// generation at the moment expire() scheduled it. The active sweep
// (Milestone 6) must delete the entry only if its live deadline and
// generation still match this snapshot -- otherwise the key was replaced
// or re-expired since, and deleting it would drop a newer value (see
// docs/CONCURRENCY.md, "TTL Races").
struct ExpirationCandidate {
  TimePoint deadline;
  std::uint64_t generation;
  std::string key;
};

// Orders a min-heap on `deadline` (soonest first): std::priority_queue is
// a max-heap by default, so this comparator is inverted.
struct ExpirationCandidateOrder {
  [[nodiscard]] bool operator()(const ExpirationCandidate& a,
                                const ExpirationCandidate& b) const noexcept {
    return a.deadline > b.deadline;
  }
};

// One shard: an independent lock plus the key/value map it guards, plus a
// heap of expiration candidates for the active sweep to consult. Reads
// take a shared lock; writes, deletes, and expiration cleanup (lazy or
// active) take an exclusive lock. Never hold `mutex` while blocking on a
// queue, doing I/O, logging a payload, or sleeping.
struct Shard {
  mutable std::shared_mutex mutex;
  std::unordered_map<std::string, Entry, TransparentStringHash, std::equal_to<>> entries;
  std::uint64_t next_generation = 0;
  std::priority_queue<ExpirationCandidate, std::vector<ExpirationCandidate>,
                      ExpirationCandidateOrder>
      expiration_heap;
};

[[nodiscard]] inline bool is_expired(const Entry& entry, TimePoint now) noexcept {
  return entry.deadline.has_value() && *entry.deadline <= now;
}

}  // namespace kvstore::storage::detail
