#include "kvstore/protocol/parser.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <random>
#include <string>
#include <string_view>

namespace kvstore::protocol {
namespace {

std::string encode_bulk(std::string_view value) {
  return "$" + std::to_string(value.size()) + "\r\n" + std::string(value) + "\r\n";
}

std::string encode_frame(std::initializer_list<std::string_view> parts) {
  std::string result = "*" + std::to_string(parts.size()) + "\r\n";
  for (const std::string_view part : parts) {
    result += encode_bulk(part);
  }
  return result;
}

// Take and return by value: `result` is often a temporary produced inline
// (e.g. expect_complete(parse_command(...))), and a reference into it would
// dangle past this call's full expression once the temporary is destroyed.
ParsedCommand expect_complete(ParseResult result) {
  return std::get<ParsedCommand>(std::move(result));
}

ProtocolError expect_error(ParseResult result) {
  return std::get<ProtocolError>(std::move(result));
}

// --- Normal behavior: one of each supported command ------------------------

TEST(ParserTest, ParsesPing) {
  const std::string frame = encode_frame({"PING"});
  const ParsedCommand& parsed = expect_complete(parse_command(frame));
  EXPECT_EQ(parsed.command.type, CommandType::kPing);
  EXPECT_EQ(parsed.consumed_bytes, frame.size());
}

TEST(ParserTest, ParsesSet) {
  const std::string frame = encode_frame({"SET", "name", "Anish"});
  const ParsedCommand& parsed = expect_complete(parse_command(frame));
  EXPECT_EQ(parsed.command.type, CommandType::kSet);
  EXPECT_EQ(parsed.command.key, "name");
  EXPECT_EQ(parsed.command.value, "Anish");
  EXPECT_EQ(parsed.consumed_bytes, frame.size());
}

TEST(ParserTest, ParsesGet) {
  const std::string frame = encode_frame({"GET", "name"});
  const ParsedCommand& parsed = expect_complete(parse_command(frame));
  EXPECT_EQ(parsed.command.type, CommandType::kGet);
  EXPECT_EQ(parsed.command.key, "name");
}

TEST(ParserTest, ParsesDel) {
  const ParsedCommand& parsed = expect_complete(parse_command(encode_frame({"DEL", "name"})));
  EXPECT_EQ(parsed.command.type, CommandType::kDel);
  EXPECT_EQ(parsed.command.key, "name");
}

TEST(ParserTest, ParsesExists) {
  const ParsedCommand& parsed = expect_complete(parse_command(encode_frame({"EXISTS", "name"})));
  EXPECT_EQ(parsed.command.type, CommandType::kExists);
  EXPECT_EQ(parsed.command.key, "name");
}

TEST(ParserTest, ParsesExpire) {
  const ParsedCommand& parsed =
      expect_complete(parse_command(encode_frame({"EXPIRE", "name", "10"})));
  EXPECT_EQ(parsed.command.type, CommandType::kExpire);
  EXPECT_EQ(parsed.command.key, "name");
  EXPECT_EQ(parsed.command.seconds, std::chrono::seconds{10});
}

TEST(ParserTest, ParsesTtl) {
  const ParsedCommand& parsed = expect_complete(parse_command(encode_frame({"TTL", "name"})));
  EXPECT_EQ(parsed.command.type, CommandType::kTtl);
  EXPECT_EQ(parsed.command.key, "name");
}

TEST(ParserTest, CommandNamesAreCaseInsensitive) {
  EXPECT_EQ(expect_complete(parse_command(encode_frame({"ping"}))).command.type,
            CommandType::kPing);
  EXPECT_EQ(expect_complete(parse_command(encode_frame({"Set", "k", "v"}))).command.type,
            CommandType::kSet);
  EXPECT_EQ(expect_complete(parse_command(encode_frame({"gEt", "k"}))).command.type,
            CommandType::kGet);
}

TEST(ParserTest, ExpireAcceptsZeroSeconds) {
  const ParsedCommand& parsed = expect_complete(parse_command(encode_frame({"EXPIRE", "k", "0"})));
  EXPECT_EQ(parsed.command.seconds, std::chrono::seconds{0});
}

// --- Every split point of a representative frame ----------------------------

TEST(ParserTest, EverySplitPointOfAFrameReportsNeedMoreDataUntilComplete) {
  const std::string frame = encode_frame({"SET", "name", "Anish"});
  for (std::size_t prefix_len = 0; prefix_len < frame.size(); ++prefix_len) {
    const ParseResult result = parse_command(std::string_view(frame).substr(0, prefix_len));
    ASSERT_TRUE(std::holds_alternative<NeedMoreData>(result)) << "prefix_len=" << prefix_len;
  }
  const ParsedCommand& parsed = expect_complete(parse_command(frame));
  EXPECT_EQ(parsed.consumed_bytes, frame.size());
}

TEST(ParserTest, EverySplitPointOfPingReportsNeedMoreDataUntilComplete) {
  const std::string frame = encode_frame({"PING"});
  for (std::size_t prefix_len = 0; prefix_len < frame.size(); ++prefix_len) {
    const ParseResult result = parse_command(std::string_view(frame).substr(0, prefix_len));
    ASSERT_TRUE(std::holds_alternative<NeedMoreData>(result)) << "prefix_len=" << prefix_len;
  }
}

// --- Multiple pipelined frames -----------------------------------------------

TEST(ParserTest, ParsesPipelinedFrames) {
  const std::string ping_frame = encode_frame({"PING"});
  const std::string set_frame = encode_frame({"SET", "k", "v"});
  const std::string get_frame = encode_frame({"GET", "k"});
  const std::string buffer = ping_frame + set_frame + get_frame;

  std::string_view remaining = buffer;

  const ParsedCommand& first = expect_complete(parse_command(remaining));
  EXPECT_EQ(first.command.type, CommandType::kPing);
  EXPECT_EQ(first.consumed_bytes, ping_frame.size());
  remaining.remove_prefix(first.consumed_bytes);

  const ParsedCommand& second = expect_complete(parse_command(remaining));
  EXPECT_EQ(second.command.type, CommandType::kSet);
  EXPECT_EQ(second.consumed_bytes, set_frame.size());
  remaining.remove_prefix(second.consumed_bytes);

  const ParsedCommand& third = expect_complete(parse_command(remaining));
  EXPECT_EQ(third.command.type, CommandType::kGet);
  EXPECT_EQ(third.consumed_bytes, get_frame.size());
  remaining.remove_prefix(third.consumed_bytes);

  EXPECT_TRUE(remaining.empty());
}

TEST(ParserTest, ParsesPipelinedFramesSplitMidSecondFrame) {
  const std::string first_frame = encode_frame({"PING"});
  const std::string second_frame = encode_frame({"GET", "k"});
  // Buffer contains a complete first frame and only part of the second.
  const std::string buffer = first_frame + second_frame.substr(0, second_frame.size() - 3);

  std::string_view remaining = buffer;
  const ParsedCommand& first = expect_complete(parse_command(remaining));
  EXPECT_EQ(first.command.type, CommandType::kPing);
  remaining.remove_prefix(first.consumed_bytes);

  EXPECT_TRUE(std::holds_alternative<NeedMoreData>(parse_command(remaining)));
}

// --- Malformed frames (unrecoverable) ---------------------------------------

TEST(ParserTest, RejectsMissingArrayMarker) {
  const ProtocolError& error = expect_error(parse_command("PING\r\n"));
  EXPECT_EQ(error.category, ProtocolErrorCategory::kMalformedFrame);
  EXPECT_FALSE(error.recoverable);
}

TEST(ParserTest, RejectsNegativeArrayLength) {
  const ProtocolError& error = expect_error(parse_command("*-1\r\n"));
  EXPECT_EQ(error.category, ProtocolErrorCategory::kMalformedFrame);
  EXPECT_FALSE(error.recoverable);
}

TEST(ParserTest, RejectsNonNumericArrayLength) {
  const ProtocolError& error = expect_error(parse_command("*a\r\n"));
  EXPECT_EQ(error.category, ProtocolErrorCategory::kMalformedFrame);
  EXPECT_FALSE(error.recoverable);
}

TEST(ParserTest, RejectsArrayLengthAboveMax) {
  const std::string frame = encode_frame({"SET", "k", "v", "extra"});  // 4 elements, max is 3
  const ProtocolError& error = expect_error(parse_command(frame));
  EXPECT_EQ(error.category, ProtocolErrorCategory::kMalformedFrame);
  EXPECT_FALSE(error.recoverable);
}

TEST(ParserTest, RejectsWrongElementTypeMarker) {
  const ProtocolError& error = expect_error(parse_command("*1\r\n:3\r\n"));
  EXPECT_EQ(error.category, ProtocolErrorCategory::kMalformedFrame);
  EXPECT_FALSE(error.recoverable);
}

TEST(ParserTest, RejectsNegativeBulkLength) {
  const ProtocolError& error = expect_error(parse_command("*1\r\n$-5\r\n"));
  EXPECT_EQ(error.category, ProtocolErrorCategory::kMalformedFrame);
  EXPECT_FALSE(error.recoverable);
}

TEST(ParserTest, RejectsMissingBulkTerminator) {
  const ProtocolError& error = expect_error(parse_command("*1\r\n$4\r\nPINGXX"));
  EXPECT_EQ(error.category, ProtocolErrorCategory::kMalformedFrame);
  EXPECT_FALSE(error.recoverable);
}

TEST(ParserTest, RejectsOverlongArrayHeaderLine) {
  const ProtocolError& error = expect_error(parse_command("*111111111111111111111\r\n"));
  EXPECT_EQ(error.category, ProtocolErrorCategory::kMalformedFrame);
  EXPECT_FALSE(error.recoverable);
}

TEST(ParserTest, RejectsOverlongBulkHeaderLine) {
  const ProtocolError& error = expect_error(parse_command("*1\r\n$111111111111111111111\r\n"));
  EXPECT_EQ(error.category, ProtocolErrorCategory::kMalformedFrame);
  EXPECT_FALSE(error.recoverable);
}

TEST(ParserTest, TruncatedBulkBodyIsNeedMoreDataNotAnError) {
  // Declares 5 bytes but only 2 are present yet.
  EXPECT_TRUE(std::holds_alternative<NeedMoreData>(parse_command("*1\r\n$5\r\nPI")));
}

TEST(ParserTest, RejectsBulkLengthAboveHardCap) {
  const std::string frame = "*1\r\n$1048577\r\n";  // 1 MiB + 1, declared only; body withheld
  const ProtocolError& error = expect_error(parse_command(frame));
  EXPECT_EQ(error.category, ProtocolErrorCategory::kFieldTooLarge);
  EXPECT_FALSE(error.recoverable);
}

TEST(ParserTest, RejectsOversizedSetValue) {
  const std::string oversized_value(1024 * 1024 + 1, 'x');
  const std::string full_frame =
      "*3\r\n" + encode_bulk("SET") + encode_bulk("k") + encode_bulk(oversized_value);
  const ProtocolError& error = expect_error(parse_command(full_frame));
  EXPECT_EQ(error.category, ProtocolErrorCategory::kFieldTooLarge);
  EXPECT_FALSE(error.recoverable);
}

// --- Semantic errors (recoverable: caller can resynchronize) ----------------

TEST(ParserTest, RejectsWrongArityForSet) {
  const std::string frame = encode_frame({"SET", "k"});
  const ProtocolError& error = expect_error(parse_command(frame));
  EXPECT_EQ(error.category, ProtocolErrorCategory::kWrongArity);
  EXPECT_TRUE(error.recoverable);
  EXPECT_EQ(error.consumed_bytes, frame.size());
}

TEST(ParserTest, RejectsWrongArityForPing) {
  const std::string frame = encode_frame({"PING", "extra"});
  const ProtocolError& error = expect_error(parse_command(frame));
  EXPECT_EQ(error.category, ProtocolErrorCategory::kWrongArity);
  EXPECT_TRUE(error.recoverable);
  EXPECT_EQ(error.consumed_bytes, frame.size());
}

TEST(ParserTest, RejectsUnknownCommand) {
  const std::string frame = encode_frame({"FOO", "bar"});
  const ProtocolError& error = expect_error(parse_command(frame));
  EXPECT_EQ(error.category, ProtocolErrorCategory::kUnknownCommand);
  EXPECT_TRUE(error.recoverable);
  EXPECT_EQ(error.consumed_bytes, frame.size());
}

TEST(ParserTest, EmptyArrayIsAnUnknownCommand) {
  const std::string frame = "*0\r\n";
  const ProtocolError& error = expect_error(parse_command(frame));
  EXPECT_EQ(error.category, ProtocolErrorCategory::kUnknownCommand);
  EXPECT_TRUE(error.recoverable);
  EXPECT_EQ(error.consumed_bytes, frame.size());
}

TEST(ParserTest, RejectsNonIntegerExpireSeconds) {
  const std::string frame = encode_frame({"EXPIRE", "k", "soon"});
  const ProtocolError& error = expect_error(parse_command(frame));
  EXPECT_EQ(error.category, ProtocolErrorCategory::kInvalidInteger);
  EXPECT_TRUE(error.recoverable);
  EXPECT_EQ(error.consumed_bytes, frame.size());
}

TEST(ParserTest, RejectsNegativeExpireSeconds) {
  const std::string frame = encode_frame({"EXPIRE", "k", "-5"});
  const ProtocolError& error = expect_error(parse_command(frame));
  EXPECT_EQ(error.category, ProtocolErrorCategory::kInvalidInteger);
  EXPECT_TRUE(error.recoverable);
}

TEST(ParserTest, RejectsOverflowingExpireSeconds) {
  const std::string frame = encode_frame({"EXPIRE", "k", "99999999999999999999"});
  const ProtocolError& error = expect_error(parse_command(frame));
  EXPECT_EQ(error.category, ProtocolErrorCategory::kInvalidInteger);
  EXPECT_TRUE(error.recoverable);
}

TEST(ParserTest, RejectsOversizedKeyButStaysRecoverable) {
  const std::string oversized_key(600, 'k');  // over the 512-byte limit, under the 1 MiB hard cap
  const std::string frame = encode_frame({"GET", oversized_key});
  const ProtocolError& error = expect_error(parse_command(frame));
  EXPECT_EQ(error.category, ProtocolErrorCategory::kFieldTooLarge);
  EXPECT_TRUE(error.recoverable);
  EXPECT_EQ(error.consumed_bytes, frame.size());
}

// --- Fuzz / property-style: malformed lengths and truncated input -----------

TEST(ParserTest, RandomTruncationsOfAValidFrameNeverCrash) {
  const std::string frame = encode_frame({"EXPIRE", "some-key", "42"});
  for (std::size_t len = 0; len <= frame.size(); ++len) {
    const ParseResult result = parse_command(std::string_view(frame).substr(0, len));
    if (len < frame.size()) {
      EXPECT_TRUE(std::holds_alternative<NeedMoreData>(result)) << "len=" << len;
    } else {
      EXPECT_TRUE(std::holds_alternative<ParsedCommand>(result));
    }
  }
}

TEST(ParserTest, RandomGarbageNeverCrashes) {
  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> byte_dist(0, 255);
  std::uniform_int_distribution<int> length_dist(0, 64);

  for (int iteration = 0; iteration < 2000; ++iteration) {
    const int length = length_dist(rng);
    std::string garbage;
    garbage.reserve(static_cast<std::size_t>(length));
    for (int i = 0; i < length; ++i) {
      garbage.push_back(static_cast<char>(byte_dist(rng)));
    }
    // The only requirement is that this returns one of the three ParseResult
    // alternatives without crashing, throwing, or reading out of bounds
    // (verified under ASan/UBSan/TSan in CI).
    const ParseResult result = parse_command(garbage);
    EXPECT_TRUE(std::holds_alternative<ParsedCommand>(result) ||
                std::holds_alternative<NeedMoreData>(result) ||
                std::holds_alternative<ProtocolError>(result));
  }
}

TEST(ParserTest, RandomBitFlipsOfAValidFrameNeverCrash) {
  const std::string base_frame = encode_frame({"SET", "some-key", "some-value"});
  std::mt19937 rng(6789);
  std::uniform_int_distribution<std::size_t> pos_dist(0, base_frame.size() - 1);
  std::uniform_int_distribution<int> bit_dist(0, 7);

  for (int iteration = 0; iteration < 500; ++iteration) {
    std::string mutated = base_frame;
    mutated[pos_dist(rng)] ^= static_cast<char>(1 << bit_dist(rng));
    const ParseResult result = parse_command(mutated);
    EXPECT_TRUE(std::holds_alternative<ParsedCommand>(result) ||
                std::holds_alternative<NeedMoreData>(result) ||
                std::holds_alternative<ProtocolError>(result));
  }
}

}  // namespace
}  // namespace kvstore::protocol
