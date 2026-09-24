#include "kvstore/protocol/serializer.hpp"

#include <gtest/gtest.h>

#include <string>

namespace kvstore::protocol {
namespace {

TEST(SerializerTest, SimpleString) { EXPECT_EQ(encode_simple_string("OK"), "+OK\r\n"); }

TEST(SerializerTest, SimpleStringPong) { EXPECT_EQ(encode_simple_string("PONG"), "+PONG\r\n"); }

TEST(SerializerTest, Error) {
  EXPECT_EQ(encode_error("ERR unknown command"), "-ERR unknown command\r\n");
}

TEST(SerializerTest, IntegerOne) { EXPECT_EQ(encode_integer(1), ":1\r\n"); }

TEST(SerializerTest, IntegerZero) { EXPECT_EQ(encode_integer(0), ":0\r\n"); }

TEST(SerializerTest, IntegerNegativeOne) { EXPECT_EQ(encode_integer(-1), ":-1\r\n"); }

TEST(SerializerTest, IntegerNegativeTwo) { EXPECT_EQ(encode_integer(-2), ":-2\r\n"); }

TEST(SerializerTest, BulkString) { EXPECT_EQ(encode_bulk_string("value"), "$5\r\nvalue\r\n"); }

TEST(SerializerTest, EmptyBulkString) { EXPECT_EQ(encode_bulk_string(""), "$0\r\n\r\n"); }

TEST(SerializerTest, BulkStringIsBinarySafe) {
  const std::string value("a\r\n\0b", 5);
  const std::string expected("$5\r\na\r\n\0b\r\n", 11);
  EXPECT_EQ(encode_bulk_string(value), expected);
}

TEST(SerializerTest, NullBulkString) { EXPECT_EQ(encode_null_bulk_string(), "$-1\r\n"); }

}  // namespace
}  // namespace kvstore::protocol
