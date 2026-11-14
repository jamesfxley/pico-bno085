// SPDX-License-Identifier: MIT
// Compile the umbrella header for analysis and check its version constants.
#include "pico_bno085/pico_bno085.hpp"

#include <gtest/gtest.h>

// Compare with the build's version, not a literal copied from the header.
TEST(PicoBno085, VersionMatchesTheBuildSystem) {
    EXPECT_EQ(pico_bno085::VERSION_MAJOR, PICO_BNO085_CMAKE_VERSION_MAJOR);
    EXPECT_EQ(pico_bno085::VERSION_MINOR, PICO_BNO085_CMAKE_VERSION_MINOR);
    EXPECT_EQ(pico_bno085::VERSION_PATCH, PICO_BNO085_CMAKE_VERSION_PATCH);
}
