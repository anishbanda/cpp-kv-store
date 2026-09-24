#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

#include "kvstore/protocol/command.hpp"

namespace kvstore::protocol {

enum class ProtocolErrorCategory : std::uint8_t {
  // The bytes do not follow the supported RESP2 multi-bulk grammar (wrong
  // leading type, bad/negative length digits, missing CRLF, too many
  // declared array elements, or a field whose declared length exceeds the
  // hard per-field cap before its body is read). Framing cannot be trusted.
  kMalformedFrame,
  // A field was fully read (its declared length was within the hard cap)
  // but exceeds a command-specific limit, e.g. a key over 512 bytes.
  kFieldTooLarge,
  // The command name is recognized but the argument count is wrong.
  kWrongArity,
  // The command name is not one of the seven Version 1 commands.
  kUnknownCommand,
  // An integer argument (EXPIRE's `seconds`) is not a valid base-10,
  // nonnegative, 64-bit integer.
  kInvalidInteger,
};

// A frame the parser has fully read from the front of the input.
struct ParsedCommand {
  Command command;
  std::size_t consumed_bytes;
};

// Not enough bytes are buffered yet to determine the next frame. The
// caller should not advance its buffer and should retry once more data
// arrives.
struct NeedMoreData {};

struct ProtocolError {
  ProtocolErrorCategory category;
  std::string message;  // short, human-readable; never contains client payload bytes

  // True if the frame was well-formed enough that `consumed_bytes` is
  // known, so the caller can discard exactly that many bytes, reply with
  // an error, and keep parsing subsequent pipelined frames on the same
  // connection. False means framing itself could not be trusted; the
  // caller should stop parsing and close the connection after an optional
  // error reply (see ARCHITECTURE.md, "Error Model").
  bool recoverable = false;
  std::size_t consumed_bytes = 0;  // valid only when recoverable is true
};

using ParseResult = std::variant<ParsedCommand, NeedMoreData, ProtocolError>;

// Attempts to parse one RESP2 multi-bulk command from the front of
// `input`. Never allocates or waits based on an untrusted declared length
// before checking it against protocol limits, and never recurses (the
// grammar this project supports -- an array of bulk strings -- has no
// nesting). Does not execute the command or touch storage.
[[nodiscard]] ParseResult parse_command(std::string_view input);

}  // namespace kvstore::protocol
