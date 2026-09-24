#pragma once

namespace kvstore::net {

// Owns exactly one POSIX file descriptor and closes it on destruction.
// Move-only -- never an owning raw pointer (see AGENTS.md).
class FileDescriptor {
 public:
  FileDescriptor() noexcept = default;
  explicit FileDescriptor(int fd) noexcept : fd_(fd) {}
  ~FileDescriptor();

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  FileDescriptor(FileDescriptor&& other) noexcept;
  FileDescriptor& operator=(FileDescriptor&& other) noexcept;

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

  // Closes the currently held descriptor, if any, and takes ownership of
  // `new_fd` (default: none). Idempotent.
  void reset(int new_fd = -1) noexcept;

  // Releases ownership without closing, returning the raw descriptor. The
  // caller becomes responsible for it.
  [[nodiscard]] int release() noexcept;

 private:
  int fd_ = -1;
};

}  // namespace kvstore::net
