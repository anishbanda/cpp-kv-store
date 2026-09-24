#include "kvstore/protocol/serializer.hpp"

namespace kvstore::protocol {

std::string encode_simple_string(std::string_view value) {
  std::string result;
  result.reserve(value.size() + 3);
  result.push_back('+');
  result.append(value);
  result.append("\r\n");
  return result;
}

std::string encode_error(std::string_view message) {
  std::string result;
  result.reserve(message.size() + 3);
  result.push_back('-');
  result.append(message);
  result.append("\r\n");
  return result;
}

std::string encode_integer(std::int64_t value) {
  std::string result = ":";
  result += std::to_string(value);
  result += "\r\n";
  return result;
}

std::string encode_bulk_string(std::string_view value) {
  std::string result;
  result.reserve(value.size() + 16);
  result.push_back('$');
  result.append(std::to_string(value.size()));
  result.append("\r\n");
  result.append(value);
  result.append("\r\n");
  return result;
}

std::string encode_null_bulk_string() { return "$-1\r\n"; }

}  // namespace kvstore::protocol
