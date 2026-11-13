// SPDX-License-Identifier: MIT
#pragma once

/// \file strap_plan.hpp
/// Reset and strap ordering used by ControlPins::init() (HR-3).

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

/// Plan strap writes with NRST asserted before any strap changes.
/// SA0 is driven only for I2C because SPI uses the pin for MOSI.
/// \param sa0_high the I2C address strap level; unused for SPI.
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
