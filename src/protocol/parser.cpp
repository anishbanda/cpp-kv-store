#include "kvstore/protocol/parser.hpp"

#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace kvstore::protocol {

namespace {

// SPEC.md: 512-byte keys, 1 MiB values. The generic RESP field cap equals
// the largest legitimate field (a SET value) so no field is ever buffered
// past that size before being checked.
constexpr std::size_t kMaxKeyLength = 512;
constexpr std::size_t kMaxValueLength = 1024 * 1024;
constexpr std::size_t kMaxBulkStringLength = kMaxValueLength;

// V1's largest command (SET key value / EXPIRE key seconds) has 3
// elements. Declaring more than this is rejected before any element is
// read, the same way an oversized bulk length is.
constexpr std::size_t kMaxArrayElements = 3;

// Generous bound for a "*N" / "$N" header line: INT64_MAX is 19 digits.
constexpr std::size_t kMaxHeaderLineLength = 20;

enum class LineStatus : std::uint8_t { kFound, kNeedMoreData, kTooLong };

struct LineResult {
  LineStatus status;
  std::string_view content;  // header text before CRLF; valid iff status == kFound
  std::size_t consumed = 0;  // bytes including the CRLF; valid iff status == kFound
};

// Scans (the currently buffered) `input` for a CRLF-terminated line, bounded
// by `max_length` bytes of content so a client that never sends a
// terminator cannot make the parser buffer unbounded data waiting for one.
[[nodiscard]] LineResult read_line(std::string_view input, std::size_t max_length) {
  const std::size_t pos = input.find("\r\n");
  if (pos != std::string_view::npos) {
    if (pos > max_length) {
      return LineResult{.status = LineStatus::kTooLong, .content = {}, .consumed = 0};
    }
    return LineResult{
        .status = LineStatus::kFound, .content = input.substr(0, pos), .consumed = pos + 2};
  }
  if (input.size() > max_length) {
    return LineResult{.status = LineStatus::kTooLong, .content = {}, .consumed = 0};
  }
  return LineResult{.status = LineStatus::kNeedMoreData, .content = {}, .consumed = 0};
}

// Parses a base-10, nonnegative, 64-bit integer with no sign and no
// separators. Used for RESP length headers and for EXPIRE's `seconds`.
[[nodiscard]] std::optional<std::int64_t> parse_nonnegative_integer(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  std::int64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    const int digit = c - '0';
    if (value > (std::numeric_limits<std::int64_t>::max() - digit) / 10) {
      return std::nullopt;  // overflow
    }
    value = value * 10 + digit;
  }
  return value;
}

[[nodiscard]] std::string to_upper_ascii(std::string_view text) {
  std::string result(text);
  for (char& c : result) {
    if (c >= 'a' && c <= 'z') {
      c = static_cast<char>(c - 'a' + 'A');
    }
  }
  return result;
}

// --- Phase 1: generic RESP2 multi-bulk framing -----------------------------
// Reads an array of bulk strings into owned byte strings. Knows nothing
// about commands.

struct FrameArgs {
  std::vector<std::string> args;
  std::size_t consumed_bytes;
};

using FrameResult = std::variant<FrameArgs, NeedMoreData, ProtocolError>;

[[nodiscard]] ProtocolError malformed(std::string message) {
  return ProtocolError{.category = ProtocolErrorCategory::kMalformedFrame,
                       .message = std::move(message),
                       .recoverable = false,
                       .consumed_bytes = 0};
}

[[nodiscard]] FrameResult read_multibulk_frame(std::string_view input) {
  if (input.empty()) {
    return NeedMoreData{};
  }
  if (input.front() != '*') {
    return malformed("expected RESP array ('*')");
  }

  const LineResult array_header = read_line(input.substr(1), kMaxHeaderLineLength);
  if (array_header.status == LineStatus::kNeedMoreData) {
    return NeedMoreData{};
  }
  if (array_header.status == LineStatus::kTooLong) {
    return malformed("array length header too long");
  }

  const std::optional<std::int64_t> array_len = parse_nonnegative_integer(array_header.content);
  if (!array_len.has_value()) {
    return malformed("invalid array length");
  }
  if (static_cast<std::uint64_t>(*array_len) > kMaxArrayElements) {
    return malformed("too many command arguments");
  }

  std::size_t consumed = 1 + array_header.consumed;  // '*' plus the header line
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(*array_len));

  for (std::int64_t i = 0; i < *array_len; ++i) {
    if (consumed >= input.size()) {
      return NeedMoreData{};
    }
    if (input[consumed] != '$') {
      return malformed("expected bulk string ('$')");
    }

    const LineResult bulk_header = read_line(input.substr(consumed + 1), kMaxHeaderLineLength);
    if (bulk_header.status == LineStatus::kNeedMoreData) {
      return NeedMoreData{};
    }
    if (bulk_header.status == LineStatus::kTooLong) {
      return malformed("bulk string length header too long");
    }

    const std::optional<std::int64_t> bulk_len = parse_nonnegative_integer(bulk_header.content);
    if (!bulk_len.has_value()) {
      return malformed("invalid bulk string length");
    }
    if (static_cast<std::uint64_t>(*bulk_len) > kMaxBulkStringLength) {
      return ProtocolError{.category = ProtocolErrorCategory::kFieldTooLarge,
                           .message = "bulk string exceeds the maximum field size",
                           .recoverable = false,
                           .consumed_bytes = 0};
    }

    consumed += 1 + bulk_header.consumed;  // '$' plus the header line

    const auto body_len = static_cast<std::size_t>(*bulk_len);
    if (input.size() < consumed + body_len + 2) {
      return NeedMoreData{};
    }
    if (input[consumed + body_len] != '\r' || input[consumed + body_len + 1] != '\n') {
      return malformed("bulk string missing terminator");
    }

    args.emplace_back(input.substr(consumed, body_len));
    consumed += body_len + 2;
  }

  return FrameArgs{.args = std::move(args), .consumed_bytes = consumed};
}

// --- Phase 2: command-specific semantic validation --------------------------
// Arity, integer parsing, and size limits, applied once the frame's exact
// byte length is already known.

struct SemanticError {
  ProtocolErrorCategory category;
  std::string message;
};

using CommandResult = std::variant<Command, SemanticError>;

// `name` must already be the uppercased command name.
[[nodiscard]] SemanticError arity_error(std::string_view name) {
  return SemanticError{ProtocolErrorCategory::kWrongArity,
                       "ERR wrong number of arguments for '" + std::string(name) + "' command"};
}

[[nodiscard]] std::optional<SemanticError> check_key_length(std::string_view key) {
  if (key.size() > kMaxKeyLength) {
    return SemanticError{ProtocolErrorCategory::kFieldTooLarge, "ERR key exceeds maximum size"};
  }
  return std::nullopt;
}

[[nodiscard]] CommandResult build_command(const std::vector<std::string>& args) {
  if (args.empty()) {
    return SemanticError{ProtocolErrorCategory::kUnknownCommand, "ERR unknown command ''"};
  }

  const std::string name = to_upper_ascii(args[0]);

  if (name == "PING") {
    if (args.size() != 1) {
      return arity_error(name);
    }
    return Command{.type = CommandType::kPing, .key = {}, .value = {}, .seconds = {}};
  }

  if (name == "SET") {
    if (args.size() != 3) {
      return arity_error(name);
    }
    if (const auto err = check_key_length(args[1])) {
      return *err;
    }
    if (args[2].size() > kMaxValueLength) {
      return SemanticError{ProtocolErrorCategory::kFieldTooLarge, "ERR value exceeds maximum size"};
    }
    return Command{.type = CommandType::kSet, .key = args[1], .value = args[2], .seconds = {}};
  }

  if (name == "GET") {
    if (args.size() != 2) {
      return arity_error(name);
    }
    if (const auto err = check_key_length(args[1])) {
      return *err;
    }
    return Command{.type = CommandType::kGet, .key = args[1], .value = {}, .seconds = {}};
  }

  if (name == "DEL") {
    if (args.size() != 2) {
      return arity_error(name);
    }
    if (const auto err = check_key_length(args[1])) {
      return *err;
    }
    return Command{.type = CommandType::kDel, .key = args[1], .value = {}, .seconds = {}};
  }

  if (name == "EXISTS") {
    if (args.size() != 2) {
      return arity_error(name);
    }
    if (const auto err = check_key_length(args[1])) {
      return *err;
    }
    return Command{.type = CommandType::kExists, .key = args[1], .value = {}, .seconds = {}};
  }

  if (name == "EXPIRE") {
    if (args.size() != 3) {
      return arity_error(name);
    }
    if (const auto err = check_key_length(args[1])) {
      return *err;
    }
    const std::optional<std::int64_t> seconds = parse_nonnegative_integer(args[2]);
    if (!seconds.has_value()) {
      return SemanticError{ProtocolErrorCategory::kInvalidInteger,
                           "ERR value is not an integer or out of range"};
    }
    return Command{.type = CommandType::kExpire,
                   .key = args[1],
                   .value = {},
                   .seconds = std::chrono::seconds{*seconds}};
  }

  if (name == "TTL") {
    if (args.size() != 2) {
      return arity_error(name);
    }
    if (const auto err = check_key_length(args[1])) {
      return *err;
    }
    return Command{.type = CommandType::kTtl, .key = args[1], .value = {}, .seconds = {}};
  }

  return SemanticError{ProtocolErrorCategory::kUnknownCommand,
                       "ERR unknown command '" + args[0] + "'"};
}

}  // namespace

ParseResult parse_command(std::string_view input) {
  FrameResult frame = read_multibulk_frame(input);

  if (std::holds_alternative<NeedMoreData>(frame)) {
    return NeedMoreData{};
  }
  if (std::holds_alternative<ProtocolError>(frame)) {
    return std::get<ProtocolError>(std::move(frame));
  }

  FrameArgs& parsed_frame = std::get<FrameArgs>(frame);
  CommandResult command_result = build_command(parsed_frame.args);

  if (std::holds_alternative<SemanticError>(command_result)) {
    const SemanticError& error = std::get<SemanticError>(command_result);
    return ProtocolError{.category = error.category,
                         .message = error.message,
                         .recoverable = true,
                         .consumed_bytes = parsed_frame.consumed_bytes};
  }

  return ParsedCommand{.command = std::get<Command>(std::move(command_result)),
                       .consumed_bytes = parsed_frame.consumed_bytes};
}

}  // namespace kvstore::protocol
