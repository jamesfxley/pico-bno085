// SPDX-License-Identifier: MIT
#pragma once

// pico-bno085 umbrella header: the user-facing BNO085 driver for the Raspberry
// Pi Pico 2 (RP2350). Includes the platform-independent driver façade. The
// firmware transports (I2C/SPI) live under pico_bno085/hal/ and are included
// directly by firmware that constructs them.

// The umbrella re-exports the driver.
#include "pico_bno085/driver/bno085.hpp"  // IWYU pragma: export

namespace pico_bno085 {

// Semantic version of the library, mirroring project(pico_bno085 VERSION ...).
inline constexpr int VERSION_MAJOR = 0;
inline constexpr int VERSION_MINOR = 1;
inline constexpr int VERSION_PATCH = 0;

}  // namespace pico_bno085
