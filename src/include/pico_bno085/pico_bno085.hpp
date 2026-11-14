// SPDX-License-Identifier: MIT
#pragma once

// Public driver umbrella. Firmware includes transports from pico_bno085/hal/.

#include "pico_bno085/driver/bno085.hpp"  // IWYU pragma: export

namespace pico_bno085 {

// Semantic version of the library, mirroring project(pico_bno085 VERSION ...).
inline constexpr int VERSION_MAJOR = 0;
inline constexpr int VERSION_MINOR = 1;
inline constexpr int VERSION_PATCH = 1;

}  // namespace pico_bno085
