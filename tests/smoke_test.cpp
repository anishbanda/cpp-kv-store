#include <gtest/gtest.h>

#include "kvstore/version.hpp"

TEST(VersionTest, ReportsProjectVersion) { EXPECT_EQ(kvstore::version(), "0.1.0"); }
