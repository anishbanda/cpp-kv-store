#include "kvstore/storage/store.hpp"

#include <bit>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <utility>

#include "shard.hpp"

namespace kvstore::storage {

namespace {

[[nodiscard]] std::size_t normalize_shard_count(std::size_t requested) {
  if (requested == 0) {
    throw std::invalid_argument("Store: shard_count must be greater than zero");
  }
  // Shard selection uses a bitmask (hash & (shard_count - 1)), so a
  // requested count that is not already a power of two is rounded up to the
  // next one rather than rejected outright.
  return std::bit_ceil(requested);
}

// Looks up `key` in `shard`. If a live entry exists as of `now`, invokes
// `on_live` with it while holding only a shared lock. Otherwise -- missing
// or expired -- invokes `on_missing`, lazily removing an expired entry first
// by releasing the shared lock, reacquiring exclusively, and revalidating
// before erasing (see docs/CONCURRENCY.md, "Storage Locking").
template <typename OnLive, typename OnMissing>
void read_live(detail::Shard& shard, TimePoint now, std::string_view key, OnLive&& on_live,
               OnMissing&& on_missing) {
  {
    std::shared_lock lock(shard.mutex);
    auto it = shard.entries.find(key);
    if (it != shard.entries.end() && !detail::is_expired(it->second, now)) {
      std::forward<OnLive>(on_live)(it->second);
      return;
    }
    if (it == shard.entries.end()) {
      std::forward<OnMissing>(on_missing)();
      return;
    }
  }

  std::unique_lock lock(shard.mutex);
  auto it = shard.entries.find(key);
  if (it != shard.entries.end() && detail::is_expired(it->second, now)) {
    shard.entries.erase(it);
  }
  std::forward<OnMissing>(on_missing)();
}

}  // namespace

Store::Store(StoreOptions options) : Store(options, SystemClock::instance()) {}

Store::Store(StoreOptions options, const Clock& clock)
    : shard_count_(normalize_shard_count(options.shard_count)), clock_(clock) {
  shards_.reserve(shard_count_);
  for (std::size_t i = 0; i < shard_count_; ++i) {
    shards_.push_back(std::make_unique<detail::Shard>());
  }
}

Store::~Store() = default;

std::size_t Store::shard_count() const noexcept { return shard_count_; }

detail::Shard& Store::shard_for(std::string_view key) const noexcept {
  const std::size_t hash = std::hash<std::string_view>{}(key);
  return *shards_[hash & (shard_count_ - 1)];
}

void Store::set(std::string key, std::string value) {
  detail::Shard& shard = shard_for(key);
  std::unique_lock lock(shard.mutex);
  detail::Entry& entry = shard.entries[std::move(key)];
  entry.value = std::move(value);
  entry.deadline.reset();
  entry.generation = ++shard.next_generation;
}

std::optional<std::string> Store::get(std::string_view key) {
  detail::Shard& shard = shard_for(key);
  std::optional<std::string> result;
  read_live(
      shard, clock_.now(), key, [&result](const detail::Entry& entry) { result = entry.value; },
      [] {});
  return result;
}

bool Store::exists(std::string_view key) {
  detail::Shard& shard = shard_for(key);
  bool found = false;
  read_live(shard, clock_.now(), key, [&found](const detail::Entry&) { found = true; }, [] {});
  return found;
}

TtlResult Store::ttl(std::string_view key) {
  detail::Shard& shard = shard_for(key);
  const TimePoint now = clock_.now();
  TtlResult result;
  read_live(
      shard, now, key,
      [&result, now](const detail::Entry& entry) {
        if (!entry.deadline.has_value()) {
          result = TtlResult{.status = TtlResult::Status::kNoTtl, .remaining = std::nullopt};
          return;
        }
        result = TtlResult{
            .status = TtlResult::Status::kHasTtl,
            .remaining = std::chrono::duration_cast<std::chrono::seconds>(*entry.deadline - now)};
      },
      [] {});
  return result;
}

bool Store::del(std::string_view key) {
  detail::Shard& shard = shard_for(key);
  const TimePoint now = clock_.now();
  std::unique_lock lock(shard.mutex);
  auto it = shard.entries.find(key);
  if (it == shard.entries.end()) {
    return false;
  }
  const bool was_live = !detail::is_expired(it->second, now);
  shard.entries.erase(it);
  return was_live;
}

bool Store::expire(std::string_view key, std::chrono::seconds ttl_seconds) {
  detail::Shard& shard = shard_for(key);
  const TimePoint now = clock_.now();
  std::unique_lock lock(shard.mutex);
  auto it = shard.entries.find(key);
  if (it == shard.entries.end()) {
    return false;
  }
  if (detail::is_expired(it->second, now)) {
    shard.entries.erase(it);
    return false;
  }
  it->second.deadline = now + ttl_seconds;
  it->second.generation = ++shard.next_generation;
  return true;
}

}  // namespace kvstore::storage
