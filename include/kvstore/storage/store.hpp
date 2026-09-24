#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kvstore/storage/clock.hpp"
#include "kvstore/storage/types.hpp"

namespace kvstore::storage {

namespace detail {
struct Shard;
}  // namespace detail

// Thread-safe, sharded in-memory key-value store.
//
// Keys are distributed across a fixed, power-of-two number of shards, each
// guarded by its own std::shared_mutex. A single-key operation acquires at
// most one shard's lock, so operations on unrelated keys in different shards
// proceed concurrently. See docs/CONCURRENCY.md for the full locking model
// and ARCHITECTURE.md for how this component fits the rest of the server.
//
// Store is command- and protocol-independent: it returns plain values and
// small result types rather than RESP-formatted strings.
class Store {
 public:
  explicit Store(StoreOptions options = {});
  Store(StoreOptions options, const Clock& clock);
  ~Store();

  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;
  Store(Store&&) = delete;
  Store& operator=(Store&&) = delete;

  // Stores `value` under `key`, replacing any existing value and clearing
  // any existing expiration on that key.
  void set(std::string key, std::string value);

  // Returns the live value for `key`, or std::nullopt if it is missing or
  // has expired. An expired entry is lazily removed as a side effect.
  [[nodiscard]] std::optional<std::string> get(std::string_view key);

  // Removes `key`. Returns true if a live entry was removed, false if the
  // key was missing or already expired.
  [[nodiscard]] bool del(std::string_view key);

  // Returns true if `key` has a live (unexpired) entry.
  [[nodiscard]] bool exists(std::string_view key);

  // Sets an expiration of `ttl` from now on an existing, live key. Returns
  // false, without effect, if the key is missing or already expired. A
  // non-positive `ttl` is accepted and expires the key immediately.
  [[nodiscard]] bool expire(std::string_view key, std::chrono::seconds ttl);

  // Returns the TTL state for `key`: missing, no expiration set, or the
  // whole seconds remaining (rounded down) until expiration.
  [[nodiscard]] TtlResult ttl(std::string_view key);

  // Actively removes expired entries, complementing lazy expiration
  // (removal on access). Visits every shard, each under its own brief
  // exclusive lock (never a global lock -- SPEC.md section 7), popping at
  // most `max_items_per_shard` candidates from that shard's expiration
  // heap. A popped candidate is only actually deleted if its captured
  // deadline and generation still match the live entry; otherwise the key
  // was replaced or re-expired since, and the candidate is simply
  // discarded (docs/CONCURRENCY.md, "TTL Races"). Returns the number of
  // entries actually removed. Safe to call from any thread.
  [[nodiscard]] std::size_t sweep_expired(std::size_t max_items_per_shard);

  // Total entries removed by sweep_expired() so far, for metrics. Safe to
  // read from any thread.
  [[nodiscard]] std::uint64_t active_expiration_count() const noexcept;

  // The actual shard count in use, after rounding up to a power of two.
  [[nodiscard]] std::size_t shard_count() const noexcept;

 private:
  [[nodiscard]] detail::Shard& shard_for(std::string_view key) const noexcept;

  std::size_t shard_count_;
  std::vector<std::unique_ptr<detail::Shard>> shards_;
  const Clock& clock_;
  std::atomic<std::uint64_t> active_expirations_{0};
};

}  // namespace kvstore::storage
