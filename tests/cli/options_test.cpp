#include "kvstore/cli/options.hpp"

#include <gtest/gtest.h>

#include <stdexcept>

namespace kvstore::cli {
namespace {

TEST(OptionsTest, DefaultsMatchSpec) {
  const ServerOptions options = parse_args({});
  EXPECT_EQ(options.bind_address, "0.0.0.0");
  EXPECT_EQ(options.port, 6380);
  EXPECT_EQ(options.worker_count, 4u);
  EXPECT_EQ(options.shard_count, 32u);
  EXPECT_EQ(options.queue_capacity, 1024u);
  EXPECT_EQ(options.log_level, "info");
  EXPECT_FALSE(options.show_help);
  EXPECT_FALSE(options.show_version);
}

TEST(OptionsTest, ParsesEachFlag) {
  const ServerOptions options =
      parse_args({"--bind", "127.0.0.1", "--port", "7000", "--workers", "8", "--shards", "64",
                  "--queue-capacity", "256", "--log-level", "debug"});
  EXPECT_EQ(options.bind_address, "127.0.0.1");
  EXPECT_EQ(options.port, 7000);
  EXPECT_EQ(options.worker_count, 8u);
  EXPECT_EQ(options.shard_count, 64u);
  EXPECT_EQ(options.queue_capacity, 256u);
  EXPECT_EQ(options.log_level, "debug");
}

TEST(OptionsTest, HelpFlagSetsShowHelp) {
  EXPECT_TRUE(parse_args({"--help"}).show_help);
  EXPECT_TRUE(parse_args({"-h"}).show_help);
}

TEST(OptionsTest, VersionFlagSetsShowVersion) {
  EXPECT_TRUE(parse_args({"--version"}).show_version);
  EXPECT_TRUE(parse_args({"-v"}).show_version);
}

TEST(OptionsTest, UnknownFlagThrows) {
  EXPECT_THROW((void)parse_args({"--nope"}), std::invalid_argument);
}

TEST(OptionsTest, MissingValueThrows) {
  EXPECT_THROW((void)parse_args({"--port"}), std::invalid_argument);
  EXPECT_THROW((void)parse_args({"--bind"}), std::invalid_argument);
}

TEST(OptionsTest, NonNumericValueThrows) {
  EXPECT_THROW((void)parse_args({"--port", "not-a-number"}), std::invalid_argument);
  EXPECT_THROW((void)parse_args({"--workers", "-1"}), std::invalid_argument);
}

TEST(OptionsTest, PortOutOfRangeThrows) {
  EXPECT_THROW((void)parse_args({"--port", "70000"}), std::invalid_argument);
}

TEST(OptionsTest, InvalidLogLevelThrows) {
  EXPECT_THROW((void)parse_args({"--log-level", "verbose"}), std::invalid_argument);
}

TEST(OptionsTest, UsageTextIsNonEmpty) { EXPECT_FALSE(usage_text().empty()); }

}  // namespace
}  // namespace kvstore::cli
