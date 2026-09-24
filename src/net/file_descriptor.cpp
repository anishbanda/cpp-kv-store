#include "kvstore/net/file_descriptor.hpp"

#include <unistd.h>

namespace kvstore::net {

FileDescriptor::~FileDescriptor() { reset(); }

FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept : fd_(other.release()) {}

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept {
  if (this != &other) {
    reset(other.release());
  }
  return *this;
}

void FileDescriptor::reset(int new_fd) noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
  }
  fd_ = new_fd;
}

int FileDescriptor::release() noexcept {
  const int fd = fd_;
  fd_ = -1;
  return fd;
}

}  // namespace kvstore::net
