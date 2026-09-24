#include "kvstore/net/connection_registry.hpp"

#include <utility>

namespace kvstore::net {

Connection& ConnectionRegistry::add(FileDescriptor fd, std::string peer_address,
                                    std::size_t max_input_bytes, std::size_t max_output_bytes) {
  const ConnectionId id = fd.get();
  const ConnectionGeneration generation = next_generation_++;
  // A live entry already at this fd would mean a previous connection was
  // not closed before the fd was reused; overwrite defensively rather than
  // leak or crash (this should not happen in normal operation).
  connections_.erase(id);
  return connections_
      .try_emplace(id, id, generation, std::move(fd), std::move(peer_address), max_input_bytes,
                   max_output_bytes)
      .first->second;
}

bool ConnectionRegistry::close(ConnectionId id, ConnectionGeneration generation) {
  const auto it = connections_.find(id);
  if (it == connections_.end() || it->second.generation() != generation) {
    return false;
  }
  connections_.erase(it);
  return true;
}

Connection* ConnectionRegistry::find(ConnectionId id, ConnectionGeneration generation) noexcept {
  const auto it = connections_.find(id);
  if (it == connections_.end() || it->second.generation() != generation) {
    return nullptr;
  }
  return &it->second;
}

}  // namespace kvstore::net
