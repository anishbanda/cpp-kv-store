#pragma once

#include <cstdint>
#include <string>

#include "kvstore/net/connection_id.hpp"
#include "kvstore/protocol/command.hpp"

namespace kvstore::worker {

using RequestSequence = std::uint64_t;

// One parsed command, tagged with enough information for the event loop
// to safely route its eventual response back to the right connection (or
// discard it) even if that connection has since closed and its fd been
// reused for someone else (see ARCHITECTURE.md, Data Ownership). Moved
// from the event-loop thread onto the work queue; owned by one worker
// thread from then on.
struct WorkItem {
  net::ConnectionId connection_id;
  net::ConnectionGeneration connection_generation;
  RequestSequence sequence;
  protocol::Command command;
};

// A worker's finished, RESP-encoded response to one WorkItem. Moved from
// a worker thread onto the completion queue; owned by the event-loop
// thread from then on.
struct Completion {
  net::ConnectionId connection_id;
  net::ConnectionGeneration connection_generation;
  RequestSequence sequence;
  std::string response;
};

}  // namespace kvstore::worker
