// SPDX-License-Identifier: MIT
// Compiles the umbrella header as a translation unit so the full public surface
// is exercised by the analysis toolchain, and pins the advertised version to
// the values the build system carries.
#include "pico_bno085/pico_bno085.hpp"

#include <gtest/gtest.h>

// The advertised version and project(pico_bno085 VERSION ...) are two records
// of one fact, compared here against the value CMake holds rather than against
// a literal copied out of the header -- which would only ever compare the
// header with itself.
TEST(PicoBno085, VersionMatchesTheBuildSystem) {
    EXPECT_EQ(pico_bno085::VERSION_MAJOR, PICO_BNO085_CMAKE_VERSION_MAJOR);
    EXPECT_EQ(pico_bno085::VERSION_MINOR, PICO_BNO085_CMAKE_VERSION_MINOR);
    EXPECT_EQ(pico_bno085::VERSION_PATCH, PICO_BNO085_CMAKE_VERSION_PATCH);
}
