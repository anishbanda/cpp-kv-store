#include "kvstore/net/bounded_buffer.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>

namespace kvstore::net {
namespace {

TEST(BoundedBufferTest, ZeroCapacityThrows) {
  EXPECT_THROW(BoundedBuffer(0), std::invalid_argument);
}

TEST(BoundedBufferTest, StartsEmpty) {
  const BoundedBuffer buffer(16);
  EXPECT_TRUE(buffer.empty());
  EXPECT_EQ(buffer.size(), 0u);
  EXPECT_EQ(buffer.capacity(), 16u);
}

TEST(BoundedBufferTest, AppendWithinCapacitySucceeds) {
  BoundedBuffer buffer(16);
  EXPECT_TRUE(buffer.append("hello"));
  EXPECT_EQ(buffer.size(), 5u);
  EXPECT_EQ(buffer.view(), "hello");
}

TEST(BoundedBufferTest, AppendAccumulates) {
  BoundedBuffer buffer(16);
  ASSERT_TRUE(buffer.append("foo"));
  ASSERT_TRUE(buffer.append("bar"));
  EXPECT_EQ(buffer.view(), "foobar");
}

TEST(BoundedBufferTest, AppendExactlyAtCapacitySucceeds) {
  BoundedBuffer buffer(5);
  EXPECT_TRUE(buffer.append("hello"));
  EXPECT_EQ(buffer.size(), 5u);
}

TEST(BoundedBufferTest, AppendBeyondCapacityFailsAndLeavesBufferUnchanged) {
  BoundedBuffer buffer(5);
  ASSERT_TRUE(buffer.append("abc"));
  EXPECT_FALSE(buffer.append("defgh"));  // 3 + 5 > 5
  EXPECT_EQ(buffer.view(), "abc");
}

TEST(BoundedBufferTest, ConsumePartialRemovesFromTheFront) {
  BoundedBuffer buffer(16);
  ASSERT_TRUE(buffer.append("hello world"));
  buffer.consume(6);
  EXPECT_EQ(buffer.view(), "world");
}

TEST(BoundedBufferTest, ConsumeExactSizeEmptiesTheBuffer) {
  BoundedBuffer buffer(16);
  ASSERT_TRUE(buffer.append("hello"));
  buffer.consume(5);
  EXPECT_TRUE(buffer.empty());
}

TEST(BoundedBufferTest, ConsumeBeyondSizeClampsToEmpty) {
  BoundedBuffer buffer(16);
  ASSERT_TRUE(buffer.append("hi"));
  buffer.consume(1000);
  EXPECT_TRUE(buffer.empty());
}

TEST(BoundedBufferTest, ConsumeThenAppendAgainReclaimsCapacity) {
  BoundedBuffer buffer(5);
  ASSERT_TRUE(buffer.append("hello"));
  ASSERT_FALSE(buffer.append("x"));
  buffer.consume(5);
  EXPECT_TRUE(buffer.append("world"));
  EXPECT_EQ(buffer.view(), "world");
}

}  // namespace
}  // namespace kvstore::net
