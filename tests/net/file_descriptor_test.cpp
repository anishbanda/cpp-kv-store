#include "kvstore/net/file_descriptor.hpp"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <cerrno>
#include <utility>

namespace kvstore::net {
namespace {

TEST(FileDescriptorTest, DefaultConstructedIsInvalid) {
  const FileDescriptor fd;
  EXPECT_FALSE(fd.valid());
  EXPECT_EQ(fd.get(), -1);
}

TEST(FileDescriptorTest, WrapsAndExposesARealDescriptor) {
  int pipe_fds[2];
  ASSERT_EQ(::pipe(pipe_fds), 0);
  const FileDescriptor read_end(pipe_fds[0]);
  EXPECT_TRUE(read_end.valid());
  EXPECT_EQ(read_end.get(), pipe_fds[0]);
  ::close(pipe_fds[1]);
}

TEST(FileDescriptorTest, DestructorClosesTheDescriptor) {
  int pipe_fds[2];
  ASSERT_EQ(::pipe(pipe_fds), 0);
  const int probe_fd = pipe_fds[0];
  {
    const FileDescriptor owned(pipe_fds[0]);
    EXPECT_TRUE(owned.valid());
  }
  errno = 0;
  EXPECT_EQ(::fcntl(probe_fd, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);
  ::close(pipe_fds[1]);
}

TEST(FileDescriptorTest, MoveConstructTransfersOwnership) {
  int pipe_fds[2];
  ASSERT_EQ(::pipe(pipe_fds), 0);
  FileDescriptor a(pipe_fds[0]);
  const FileDescriptor b(std::move(a));

  EXPECT_FALSE(a.valid());
  EXPECT_TRUE(b.valid());
  EXPECT_EQ(b.get(), pipe_fds[0]);
  ::close(pipe_fds[1]);
}

TEST(FileDescriptorTest, MoveAssignClosesThePreviouslyHeldDescriptor) {
  int pipe_a[2];
  int pipe_b[2];
  ASSERT_EQ(::pipe(pipe_a), 0);
  ASSERT_EQ(::pipe(pipe_b), 0);

  const int probe_fd = pipe_a[0];
  FileDescriptor a(pipe_a[0]);
  FileDescriptor b(pipe_b[0]);
  a = std::move(b);

  EXPECT_EQ(a.get(), pipe_b[0]);
  EXPECT_FALSE(b.valid());

  errno = 0;
  EXPECT_EQ(::fcntl(probe_fd, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);

  ::close(pipe_a[1]);
  ::close(pipe_b[1]);
}

TEST(FileDescriptorTest, ReleaseTransfersOwnershipToTheCaller) {
  int pipe_fds[2];
  ASSERT_EQ(::pipe(pipe_fds), 0);
  FileDescriptor owned(pipe_fds[0]);

  const int released = owned.release();
  EXPECT_EQ(released, pipe_fds[0]);
  EXPECT_FALSE(owned.valid());

  // Ownership is now the test's; close both ends manually.
  ::close(released);
  ::close(pipe_fds[1]);
}

TEST(FileDescriptorTest, ResetClosesTheOldAndTakesTheNew) {
  int pipe_a[2];
  int pipe_b[2];
  ASSERT_EQ(::pipe(pipe_a), 0);
  ASSERT_EQ(::pipe(pipe_b), 0);

  const int probe_fd = pipe_a[0];
  FileDescriptor owned(pipe_a[0]);
  owned.reset(pipe_b[0]);

  EXPECT_EQ(owned.get(), pipe_b[0]);
  errno = 0;
  EXPECT_EQ(::fcntl(probe_fd, F_GETFD), -1);
  EXPECT_EQ(errno, EBADF);

  ::close(pipe_a[1]);
  ::close(pipe_b[1]);
}

}  // namespace
}  // namespace kvstore::net
