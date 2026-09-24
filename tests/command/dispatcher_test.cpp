#include "kvstore/command/dispatcher.hpp"

#include <gtest/gtest.h>

#include <chrono>

#include "../storage/fake_clock.hpp"
#include "kvstore/storage/store.hpp"

namespace kvstore::command {
namespace {

using protocol::Command;
using protocol::CommandType;

Command make_command(CommandType type, std::string key = "", std::string value = "",
                     std::chrono::seconds seconds = std::chrono::seconds{0}) {
  return Command{
      .type = type, .key = std::move(key), .value = std::move(value), .seconds = seconds};
}

// Uses a FakeClock, not the real system clock: TTL assertions below need
// exact remaining-second values, and no real time may elapse between a SET
// or EXPIRE call and the TTL check that follows it (see Milestone 1's
// storage tests for the same rationale).
class DispatcherTest : public ::testing::Test {
 protected:
  storage::testing::FakeClock clock_;
  storage::Store store_{storage::StoreOptions{}, clock_};
};

TEST_F(DispatcherTest, Ping) {
  EXPECT_EQ(dispatch(store_, make_command(CommandType::kPing)), "+PONG\r\n");
}

TEST_F(DispatcherTest, SetReturnsOkAndStores) {
  EXPECT_EQ(dispatch(store_, make_command(CommandType::kSet, "k", "v")), "+OK\r\n");
  EXPECT_EQ(store_.get("k"), "v");
}

TEST_F(DispatcherTest, GetOnMissingKeyReturnsNullBulkString) {
  EXPECT_EQ(dispatch(store_, make_command(CommandType::kGet, "missing")), "$-1\r\n");
}

TEST_F(DispatcherTest, GetOnExistingKeyReturnsBulkString) {
  (void)dispatch(store_, make_command(CommandType::kSet, "k", "hello"));
  EXPECT_EQ(dispatch(store_, make_command(CommandType::kGet, "k")), "$5\r\nhello\r\n");
}

TEST_F(DispatcherTest, DelOnMissingKeyReturnsZero) {
  EXPECT_EQ(dispatch(store_, make_command(CommandType::kDel, "missing")), ":0\r\n");
}

TEST_F(DispatcherTest, DelOnExistingKeyReturnsOneAndRemoves) {
  (void)dispatch(store_, make_command(CommandType::kSet, "k", "v"));
  EXPECT_EQ(dispatch(store_, make_command(CommandType::kDel, "k")), ":1\r\n");
  EXPECT_FALSE(store_.exists("k"));
}

TEST_F(DispatcherTest, ExistsReflectsLiveness) {
  EXPECT_EQ(dispatch(store_, make_command(CommandType::kExists, "k")), ":0\r\n");
  (void)dispatch(store_, make_command(CommandType::kSet, "k", "v"));
  EXPECT_EQ(dispatch(store_, make_command(CommandType::kExists, "k")), ":1\r\n");
}

TEST_F(DispatcherTest, ExpireOnMissingKeyReturnsZero) {
  EXPECT_EQ(
      dispatch(store_, make_command(CommandType::kExpire, "missing", "", std::chrono::seconds{10})),
      ":0\r\n");
}

TEST_F(DispatcherTest, ExpireOnExistingKeyReturnsOne) {
  (void)dispatch(store_, make_command(CommandType::kSet, "k", "v"));
  EXPECT_EQ(dispatch(store_, make_command(CommandType::kExpire, "k", "", std::chrono::seconds{10})),
            ":1\r\n");
}

TEST_F(DispatcherTest, TtlOnMissingKeyReturnsNegativeTwo) {
  EXPECT_EQ(dispatch(store_, make_command(CommandType::kTtl, "missing")), ":-2\r\n");
}

TEST_F(DispatcherTest, TtlOnKeyWithNoExpirationReturnsNegativeOne) {
  (void)dispatch(store_, make_command(CommandType::kSet, "k", "v"));
  EXPECT_EQ(dispatch(store_, make_command(CommandType::kTtl, "k")), ":-1\r\n");
}

TEST_F(DispatcherTest, TtlOnKeyWithExpirationReturnsRemainingSeconds) {
  (void)dispatch(store_, make_command(CommandType::kSet, "k", "v"));
  (void)dispatch(store_, make_command(CommandType::kExpire, "k", "", std::chrono::seconds{10}));
  EXPECT_EQ(dispatch(store_, make_command(CommandType::kTtl, "k")), ":10\r\n");
}

TEST_F(DispatcherTest, SetClearsAPreviousTtl) {
  (void)dispatch(store_, make_command(CommandType::kSet, "k", "v1"));
  (void)dispatch(store_, make_command(CommandType::kExpire, "k", "", std::chrono::seconds{10}));
  (void)dispatch(store_, make_command(CommandType::kSet, "k", "v2"));
  EXPECT_EQ(dispatch(store_, make_command(CommandType::kTtl, "k")), ":-1\r\n");
}

}  // namespace
}  // namespace kvstore::command
