#include "horizon/version.h"

#include <gtest/gtest.h>

// TEST(group, name) defines a test; GoogleTest finds and runs it automatically.
TEST(Version, IsZeroZeroOne) {
    // EXPECT_EQ prints both values on failure. std::string == compares characters, not pointers.
    EXPECT_EQ(horizon::version(), "0.0.1");
}
