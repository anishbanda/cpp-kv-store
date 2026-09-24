#include "kvstore/command/dispatcher.hpp"

#include <optional>

#include "kvstore/protocol/serializer.hpp"

namespace kvstore::command {

namespace {

std::string dispatch_ping() { return protocol::encode_simple_string("PONG"); }

std::string dispatch_set(storage::Store& store, const protocol::Command& command) {
  store.set(command.key, command.value);
  return protocol::encode_simple_string("OK");
}

std::string dispatch_get(storage::Store& store, const protocol::Command& command) {
  const std::optional<std::string> value = store.get(command.key);
  if (!value.has_value()) {
    return protocol::encode_null_bulk_string();
  }
  return protocol::encode_bulk_string(*value);
}

std::string dispatch_del(storage::Store& store, const protocol::Command& command) {
  const bool removed = store.del(command.key);
  return protocol::encode_integer(removed ? 1 : 0);
}

std::string dispatch_exists(storage::Store& store, const protocol::Command& command) {
  const bool exists = store.exists(command.key);
  return protocol::encode_integer(exists ? 1 : 0);
}

std::string dispatch_expire(storage::Store& store, const protocol::Command& command) {
  const bool set_ok = store.expire(command.key, command.seconds);
  return protocol::encode_integer(set_ok ? 1 : 0);
}

std::string dispatch_ttl(storage::Store& store, const protocol::Command& command) {
  const storage::TtlResult result = store.ttl(command.key);
  switch (result.status) {
    case storage::TtlResult::Status::kMissing:
      return protocol::encode_integer(-2);
    case storage::TtlResult::Status::kNoTtl:
      return protocol::encode_integer(-1);
    case storage::TtlResult::Status::kHasTtl:
      return protocol::encode_integer(result.remaining->count());
  }
  return protocol::encode_error("ERR internal error: unhandled TTL status");
}

}  // namespace

std::string dispatch(storage::Store& store, const protocol::Command& command) {
  switch (command.type) {
    case protocol::CommandType::kPing:
      return dispatch_ping();
    case protocol::CommandType::kSet:
      return dispatch_set(store, command);
    case protocol::CommandType::kGet:
      return dispatch_get(store, command);
    case protocol::CommandType::kDel:
      return dispatch_del(store, command);
    case protocol::CommandType::kExists:
      return dispatch_exists(store, command);
    case protocol::CommandType::kExpire:
      return dispatch_expire(store, command);
    case protocol::CommandType::kTtl:
      return dispatch_ttl(store, command);
  }
  return protocol::encode_error("ERR internal error: unhandled command type");
}

}  // namespace kvstore::command
