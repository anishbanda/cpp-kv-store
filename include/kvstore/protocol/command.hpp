#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace kvstore::protocol {

enum class CommandType : std::uint8_t {
  kPing,
  kSet,
  kGet,
  kDel,
  kExists,
  kExpire,
  kTtl,
};

// An owned, fully validated request produced by the parser. Independent of
// the connection's input buffer and independent of the storage engine --
// protocol code never touches kvstore::storage directly (see
// ARCHITECTURE.md). Unused fields for a given `type` are left default.
struct Command {
  CommandType type;
  std::string key;                 // unused for kPing
  std::string value;               // kSet only
  std::chrono::seconds seconds{};  // kExpire only

  friend bool operator==(const Command&, const Command&) = default;
};

}  // namespace kvstore::protocol
