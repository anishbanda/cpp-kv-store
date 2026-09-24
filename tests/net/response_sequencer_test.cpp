#include "kvstore/net/response_sequencer.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace kvstore::net {
namespace {

TEST(ResponseSequencerTest, NextRequestSequenceStartsAtZeroAndIncrements) {
  ResponseSequencer sequencer;
  EXPECT_EQ(sequencer.next_request_sequence(), 0u);
  EXPECT_EQ(sequencer.next_request_sequence(), 1u);
  EXPECT_EQ(sequencer.next_request_sequence(), 2u);
}

TEST(ResponseSequencerTest, InOrderCompletionsReleaseImmediately) {
  ResponseSequencer sequencer;
  const std::uint64_t a = sequencer.next_request_sequence();
  const std::uint64_t b = sequencer.next_request_sequence();

  EXPECT_EQ(sequencer.record(a, "A"), (std::vector<std::string>{"A"}));
  EXPECT_EQ(sequencer.record(b, "B"), (std::vector<std::string>{"B"}));
}

TEST(ResponseSequencerTest, OutOfOrderCompletionBuffersUntilGapFills) {
  ResponseSequencer sequencer;
  const std::uint64_t a = sequencer.next_request_sequence();
  const std::uint64_t b = sequencer.next_request_sequence();
  const std::uint64_t c = sequencer.next_request_sequence();

  // Worker for `c` (the third request) finishes first.
  EXPECT_TRUE(sequencer.record(c, "C").empty());
  // Worker for `b` finishes next; `a` still hasn't, so still nothing ready.
  EXPECT_TRUE(sequencer.record(b, "B").empty());
  // Worker for `a` finally finishes: the whole run releases in order.
  EXPECT_EQ(sequencer.record(a, "A"), (std::vector<std::string>{"A", "B", "C"}));
}

TEST(ResponseSequencerTest, PartialGapFillReleasesOnlyTheConsecutiveRun) {
  ResponseSequencer sequencer;
  const std::uint64_t a = sequencer.next_request_sequence();
  const std::uint64_t b = sequencer.next_request_sequence();
  (void)sequencer.next_request_sequence();  // c, deliberately never completed in this test
  const std::uint64_t d = sequencer.next_request_sequence();

  EXPECT_TRUE(sequencer.record(d, "D").empty());  // buffered: c hasn't arrived
  EXPECT_TRUE(sequencer.record(b, "B").empty());  // buffered: a hasn't arrived
  // a arrives: only a and b are consecutive from the front; c is still
  // missing, so d must stay buffered even though it already arrived.
  EXPECT_EQ(sequencer.record(a, "A"), (std::vector<std::string>{"A", "B"}));
}

TEST(ResponseSequencerTest, InFlightCountReflectsOutstandingRequests) {
  ResponseSequencer sequencer;
  EXPECT_EQ(sequencer.in_flight_count(), 0u);

  const std::uint64_t a = sequencer.next_request_sequence();
  EXPECT_EQ(sequencer.in_flight_count(), 1u);
  (void)sequencer.next_request_sequence();
  EXPECT_EQ(sequencer.in_flight_count(), 2u);

  (void)sequencer.record(a, "A");
  EXPECT_EQ(sequencer.in_flight_count(), 1u);
}

TEST(ResponseSequencerTest, InFlightCountAccountsForBufferedOutOfOrderCompletions) {
  ResponseSequencer sequencer;
  const std::uint64_t a = sequencer.next_request_sequence();
  const std::uint64_t b = sequencer.next_request_sequence();

  // b's completion has arrived and is recorded, but it cannot be released
  // (and therefore is still "in flight" from the caller's perspective)
  // until a's does too.
  (void)sequencer.record(b, "B");
  EXPECT_EQ(sequencer.in_flight_count(), 2u);

  (void)sequencer.record(a, "A");
  EXPECT_EQ(sequencer.in_flight_count(), 0u);
}

}  // namespace
}  // namespace kvstore::net
