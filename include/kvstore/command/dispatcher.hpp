#pragma once

#include <string>

#include "kvstore/protocol/command.hpp"
#include "kvstore/storage/store.hpp"

namespace kvstore::command {

// Executes `command` against `store` and returns the RESP-encoded
// response (SPEC.md section 3 / docs/PROTOCOL.md section 3). The only
// place protocol::Command values are translated into storage::Store
// calls -- kept separate from both the RESP parser and the storage
// engine (see AGENTS.md, "Keep ... command execution ... separate").
[[nodiscard]] std::string dispatch(storage::Store& store, const protocol::Command& command);

}  // namespace kvstore::command
