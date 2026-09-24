#pragma once

#include <chrono>
#include <cstddef>
#include <optional>

namespace kvstore::storage {

// Configuration accepted by Store. shard_count that is zero is rejected;
// a value that is not already a power of two is rounded up to the next one
// (see store.cpp) so shard indexing can use a bitmask instead of modulo.
struct StoreOptions {
  std::size_t shard_count = 32;
};

// Result of a TTL query. Deliberately independent of any wire protocol
// encoding -- callers translate this into command-layer responses (e.g. the
// RESP -1 / -2 sentinels) themselves.
struct TtlResult {
  enum class Status { kMissing, kNoTtl, kHasTtl };

  Status status = Status::kMissing;
  std::optional<std::chrono::seconds> remaining;
};

}  // namespace kvstore::storage
