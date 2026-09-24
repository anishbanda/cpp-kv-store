#include "kvstore/net/connection.hpp"

#include <utility>

namespace kvstore::net {

Connection::Connection(ConnectionId id, ConnectionGeneration generation, FileDescriptor fd,
                       std::string peer_address, std::size_t max_input_bytes,
                       std::size_t max_output_bytes)
    : id_(id),
      generation_(generation),
      fd_(std::move(fd)),
      peer_address_(std::move(peer_address)),
      input_(max_input_bytes),
      output_(max_output_bytes) {}

}  // namespace kvstore::net
