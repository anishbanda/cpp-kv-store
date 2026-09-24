#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace kvstore::protocol {

// RESP2 response encoders (see docs/PROTOCOL.md). Each returns a fully
// framed, ready-to-write byte sequence; callers own buffering and sending
// it. Callers are responsible for passing CRLF-free content: these values
// are always short, server-authored literals (e.g. "OK", "ERR ..."), never
// client-controlled payload bytes.
[[nodiscard]] std::string encode_simple_string(std::string_view value);
[[nodiscard]] std::string encode_error(std::string_view message);
[[nodiscard]] std::string encode_integer(std::int64_t value);
[[nodiscard]] std::string encode_bulk_string(std::string_view value);
[[nodiscard]] std::string encode_null_bulk_string();

}  // namespace kvstore::protocol
