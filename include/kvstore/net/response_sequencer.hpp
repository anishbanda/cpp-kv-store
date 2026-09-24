#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace kvstore::net {

// Assigns each request on one connection a monotonically increasing
// sequence number, then reassembles out-of-order worker completions back
// into request order before they are written to the client. Not
// thread-safe: owned by, and only ever touched from, the event-loop
// thread (see docs/CONCURRENCY.md, "Response Ordering" -- ordering is
// only required within one connection, never across connections).
class ResponseSequencer {
 public:
  [[nodiscard]] std::uint64_t next_request_sequence() noexcept { return next_request_sequence_++; }

  // Records the response for `sequence` (from a worker completion, a
  // synchronous protocol error, or a synthesized backpressure reply --
  // record() does not care which). Returns, in request order, the run of
  // responses that are now ready to write: zero if `sequence` is still
  // ahead of a gap, one for the common in-order case, or more than one if
  // this call fills a gap that unblocks previously-buffered completions.
  [[nodiscard]] std::vector<std::string> record(std::uint64_t sequence, std::string response);

  // How many requests have been assigned a sequence but not yet had a
  // response released. Used to cap in-flight requests per connection
  // (SPEC.md/ARCHITECTURE.md backpressure).
  [[nodiscard]] std::uint64_t in_flight_count() const noexcept {
    return next_request_sequence_ - next_expected_;
  }

 private:
  std::uint64_t next_request_sequence_ = 0;
  std::uint64_t next_expected_ = 0;
  std::map<std::uint64_t, std::string> pending_;
};

}  // namespace kvstore::net
