// SPDX-License-Identifier: MIT
// HR-3: the order ControlPins::init() drives the strap and reset lines in.
#include "pico_bno085/hal/detail/strap_plan.hpp"

#include <gtest/gtest.h>

#include <cstddef>

namespace {

using pico_bno085::hal::detail::strap_plan;
using pico_bno085::hal::detail::StrapPins;

constexpr StrapPins PINS{
    .nrst = 10, .ps0 = 11, .ps1 = 12, .bootn = 13, .sa0 = 14};

TEST(StrapPlan, I2cHoldsResetThenStrapsEveryLineSa0Last) {
    const auto plan = strap_plan(PINS, false, true);
    ASSERT_EQ(plan.count, 5U);
    EXPECT_EQ(plan.steps.at(0).pin, PINS.nrst);
    EXPECT_FALSE(plan.steps.at(0).high);
    EXPECT_EQ(plan.steps.at(1).pin, PINS.ps1);
    EXPECT_FALSE(plan.steps.at(1).high);
    EXPECT_EQ(plan.steps.at(2).pin, PINS.ps0);
    EXPECT_FALSE(plan.steps.at(2).high);
    EXPECT_EQ(plan.steps.at(3).pin, PINS.bootn);
    EXPECT_TRUE(plan.steps.at(3).high);
    EXPECT_EQ(plan.steps.at(4).pin, PINS.sa0);
    EXPECT_TRUE(plan.steps.at(4).high);
}

TEST(StrapPlan, Sa0TakesTheAddressLevelItIsGiven) {
    const auto plan = strap_plan(PINS, false, false);
    ASSERT_EQ(plan.count, 5U);
    EXPECT_EQ(plan.steps.at(4).pin, PINS.sa0);
    EXPECT_FALSE(plan.steps.at(4).high);
}

TEST(StrapPlan, SpiHoldsResetAndLeavesSa0ToTheTransport) {
    const auto plan = strap_plan(PINS, true, true);
    ASSERT_EQ(plan.count, 4U);
    EXPECT_EQ(plan.steps.at(0).pin, PINS.nrst);
    EXPECT_FALSE(plan.steps.at(0).high);
    EXPECT_EQ(plan.steps.at(1).pin, PINS.ps1);
    EXPECT_TRUE(plan.steps.at(1).high);
    EXPECT_EQ(plan.steps.at(2).pin, PINS.ps0);
    EXPECT_TRUE(plan.steps.at(2).high);
    EXPECT_EQ(plan.steps.at(3).pin, PINS.bootn);
    EXPECT_TRUE(plan.steps.at(3).high);
    for (std::size_t i = 0; i < plan.count; ++i) {
        EXPECT_NE(plan.steps.at(i).pin, PINS.sa0);
    }
}

// The plan is a constant expression, so the order is fixed at compile time.
static_assert(strap_plan(PINS, false, true).steps.at(0).pin == PINS.nrst);

}  // namespace
