// SPDX-License-Identifier: MIT
#include "pico_bno085/hal/duplex_write.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

// HR-4: receive the longer direction, bounded by staging; pad the shorter.
TEST(DuplexWrite, KeepsCoincidentCargoAndPadsTheShorterDirection) {
    const std::array<std::uint8_t, 4> header{12, 0, 3, 7};
    const auto plan = pico_bno085::hal::duplex_write(8, header, 32);
    EXPECT_EQ(plan.clocks, 12U);
    EXPECT_EQ(plan.received, 12U);
    EXPECT_TRUE(plan.cargo);
    EXPECT_FALSE(plan.truncated);
    EXPECT_FALSE(plan.continuation);
    EXPECT_EQ(pico_bno085::hal::duplex_write(20, header, 32).clocks, 20U);
}

TEST(DuplexWrite, BoundsLongCargoAndKeepsContinuation) {
    const std::array<std::uint8_t, 4> header{40, 0x80, 3, 7};
    const auto plan = pico_bno085::hal::duplex_write(8, header, 32);
    EXPECT_EQ(plan.clocks, 32U);
    EXPECT_EQ(plan.received, 32U);
    EXPECT_TRUE(plan.cargo);
    EXPECT_TRUE(plan.truncated);
    EXPECT_TRUE(plan.continuation);
}

TEST(DuplexWrite, NullAndReservedHeadersDoNotCreateCargo) {
    for (const auto header : {std::array<std::uint8_t, 4>{0, 0, 0, 0},
                              std::array<std::uint8_t, 4>{0xFF, 0xFF, 0, 0},
                              std::array<std::uint8_t, 4>{4, 0, 0, 0}}) {
        const auto plan = pico_bno085::hal::duplex_write(8, header, 32);
        EXPECT_EQ(plan.clocks, 8U);
        EXPECT_FALSE(plan.cargo);
        EXPECT_FALSE(plan.truncated);
    }
}
