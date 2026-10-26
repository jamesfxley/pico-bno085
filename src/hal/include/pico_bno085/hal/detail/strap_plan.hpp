// SPDX-License-Identifier: MIT
#pragma once

/// \file strap_plan.hpp
/// The order the strap and reset lines are driven in (HR-3), as data rather
/// than as SDK calls, so the order is tested on the host. ControlPins::init()
/// drives exactly these steps, in this order.

#include <array>
#include <cstddef>
#include <cstdint>

namespace pico_bno085::hal::detail {

/// One control line driven to one level.
struct StrapStep {
    std::uint32_t pin = 0;  ///< The GPIO driven.
    bool high = false;      ///< The level it is driven to.
};

/// The control lines a strap plan drives.
struct StrapPins {
    std::uint32_t nrst = 0;   ///< Active-low reset.
    std::uint32_t ps0 = 0;    ///< Protocol-select strap, then SPI WAKE.
    std::uint32_t ps1 = 0;    ///< Protocol-select strap.
    std::uint32_t bootn = 0;  ///< Boot-mode strap.
    std::uint32_t sa0 = 0;    ///< I2C address strap, or SPI MOSI.
};

/// The steps in order, and how many of `steps` are used.
struct StrapPlan {
    std::array<StrapStep, 5> steps{};  ///< The steps, first to last.
    std::size_t count = 0;             ///< How many of them apply.
};

/// The steps that strap the hub for one bus with it held in reset: NRST
/// asserted (driven low) first, so every strap settles before the hub samples
/// it at the reset edge; then PS1 and PS0 (0:0 for I2C, 1:1 for SPI) and BOOTN
/// high, to run the application image. SA0 is driven last and only for I2C:
/// in SPI mode the hub's SA0/H_MOSI pin is the SPI transport's MOSI.
///
/// \param pins the control lines.
/// \param spi whether the plan straps for SPI rather than I2C.
/// \param sa0_high the I2C address strap level; unused for SPI.
/// \return the plan.
[[nodiscard]] constexpr StrapPlan strap_plan(const StrapPins& pins, bool spi,
                                             bool sa0_high) {
    StrapPlan plan;
    const auto add = [&plan](std::uint32_t pin, bool high) {
        plan.steps.at(plan.count) = StrapStep{.pin = pin, .high = high};
        ++plan.count;
    };
    add(pins.nrst, false);
    add(pins.ps1, spi);
    add(pins.ps0, spi);
    add(pins.bootn, true);
    if (!spi) {
        add(pins.sa0, sa0_high);
    }
    return plan;
}

}  // namespace pico_bno085::hal::detail
