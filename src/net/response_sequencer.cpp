#include "kvstore/net/response_sequencer.hpp"

#include <utility>

namespace kvstore::net {

std::vector<std::string> ResponseSequencer::record(std::uint64_t sequence, std::string response) {
  pending_.emplace(sequence, std::move(response));

  std::vector<std::string> ready;
  for (auto it = pending_.find(next_expected_); it != pending_.end();
       it = pending_.find(next_expected_)) {
    ready.push_back(std::move(it->second));
    pending_.erase(it);
    ++next_expected_;
  }
  return ready;
}

}  // namespace kvstore::net
