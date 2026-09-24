#pragma once

#include <cstdint>

namespace kvstore::net {

// A connection's id is its accepted socket's fd number. The OS reuses fd
// numbers once closed, so id alone cannot tell two different clients
// apart over time; `generation` (monotonically increasing, assigned by
// ConnectionRegistry) disambiguates a stale reference to a reused id. See
// ARCHITECTURE.md, "Data Ownership".
//
// Deliberately a standalone leaf header: kvstore::worker needs these two
// types (to tag a WorkItem/Completion with the connection it belongs to)
// without depending on the rest of kvstore::net.
using ConnectionId = int;
using ConnectionGeneration = std::uint64_t;

}  // namespace kvstore::net
