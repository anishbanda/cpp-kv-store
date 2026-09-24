#include "kvstore/net/bounded_buffer.hpp"

#include <stdexcept>

namespace kvstore::net {

BoundedBuffer::BoundedBuffer(std::size_t capacity) : capacity_(capacity) {
  if (capacity_ == 0) {
    throw std::invalid_argument("BoundedBuffer: capacity must be greater than zero");
  }
}

bool BoundedBuffer::append(std::string_view data) {
  if (data.size() > capacity_ - data_.size()) {
    return false;
  }
  data_.append(data);
  return true;
}

void BoundedBuffer::consume(std::size_t count) {
  if (count >= data_.size()) {
    data_.clear();
    return;
  }
  data_.erase(0, count);
}

}  // namespace kvstore::net
