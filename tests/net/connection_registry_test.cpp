#include "kvstore/net/connection_registry.hpp"

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include "kvstore/net/file_descriptor.hpp"

namespace kvstore::net {
namespace {

int make_throwaway_socket() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  EXPECT_GE(fd, 0);
  return fd;
}

TEST(ConnectionRegistryTest, AddRegistersAConnection) {
  ConnectionRegistry registry;
  const int raw_fd = make_throwaway_socket();

  Connection& connection = registry.add(FileDescriptor(raw_fd), "127.0.0.1:1234");
  EXPECT_EQ(connection.id(), raw_fd);
  EXPECT_EQ(connection.peer_address(), "127.0.0.1:1234");
  EXPECT_EQ(registry.size(), 1u);
}

TEST(ConnectionRegistryTest, FindReturnsTheConnectionForAMatchingIdAndGeneration) {
  ConnectionRegistry registry;
  const int raw_fd = make_throwaway_socket();
  const Connection& added = registry.add(FileDescriptor(raw_fd), "peer");

  Connection* found = registry.find(added.id(), added.generation());
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->id(), added.id());
}

TEST(ConnectionRegistryTest, FindReturnsNullForAWrongGeneration) {
  ConnectionRegistry registry;
  const int raw_fd = make_throwaway_socket();
  const Connection& added = registry.add(FileDescriptor(raw_fd), "peer");

  EXPECT_EQ(registry.find(added.id(), added.generation() + 1), nullptr);
}

TEST(ConnectionRegistryTest, FindReturnsNullForAnUnknownId) {
  ConnectionRegistry registry;
  EXPECT_EQ(registry.find(999999, 1), nullptr);
}

TEST(ConnectionRegistryTest, CloseRemovesAMatchingConnection) {
  ConnectionRegistry registry;
  const int raw_fd = make_throwaway_socket();
  const Connection& added = registry.add(FileDescriptor(raw_fd), "peer");
  const ConnectionId id = added.id();
  const ConnectionGeneration generation = added.generation();

  EXPECT_TRUE(registry.close(id, generation));
  EXPECT_EQ(registry.size(), 0u);
  EXPECT_EQ(registry.find(id, generation), nullptr);
}

TEST(ConnectionRegistryTest, CloseWithWrongGenerationLeavesConnectionInPlace) {
  ConnectionRegistry registry;
  const int raw_fd = make_throwaway_socket();
  const Connection& added = registry.add(FileDescriptor(raw_fd), "peer");

  EXPECT_FALSE(registry.close(added.id(), added.generation() + 1));
  EXPECT_EQ(registry.size(), 1u);
}

TEST(ConnectionRegistryTest, CloseOfUnknownIdReturnsFalse) {
  ConnectionRegistry registry;
  EXPECT_FALSE(registry.close(999999, 1));
}

TEST(ConnectionRegistryTest, GenerationsAreMonotonicallyIncreasing) {
  ConnectionRegistry registry;
  const Connection& first = registry.add(FileDescriptor(make_throwaway_socket()), "a");
  const Connection& second = registry.add(FileDescriptor(make_throwaway_socket()), "b");
  EXPECT_LT(first.generation(), second.generation());
}

// Deterministically forces two different logical connections to share the
// same fd number (via dup2), the exact scenario `generation` exists to
// disambiguate (see ARCHITECTURE.md, Data Ownership).
TEST(ConnectionRegistryTest, StaleGenerationIsRejectedAfterFdReuse) {
  ConnectionRegistry registry;

  const int throwaway = make_throwaway_socket();
  const int target_fd = throwaway;  // remember the specific fd number to force reuse onto

  const Connection& first = registry.add(FileDescriptor(target_fd), "first-client");
  const ConnectionId id = first.id();
  const ConnectionGeneration first_generation = first.generation();
  ASSERT_EQ(id, target_fd);

  ASSERT_TRUE(registry.close(id, first_generation));  // actually closes target_fd

  const int replacement = make_throwaway_socket();
  ASSERT_NE(::dup2(replacement, target_fd), -1);  // forces the new socket onto the same fd number
  ::close(replacement);

  const Connection& second = registry.add(FileDescriptor(target_fd), "second-client");
  const ConnectionGeneration second_generation = second.generation();
  ASSERT_EQ(second.id(), target_fd);
  ASSERT_NE(first_generation, second_generation);

  // The stale (id, generation) from the first connection must not resolve
  // to the second client that now occupies the same fd number.
  EXPECT_EQ(registry.find(id, first_generation), nullptr);
  EXPECT_FALSE(registry.close(id, first_generation));

  // The current generation still works correctly.
  Connection* found = registry.find(id, second_generation);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->peer_address(), "second-client");
}

}  // namespace
}  // namespace kvstore::net
