// SPDX-License-Identifier: MIT
#include "pico_bno085/hal/transport_core.hpp"

#include <hardware/timer.h>

#include <cstdint>

namespace pico_bno085::hal {

std::uint64_t TransportCore::now_us() { return time_us_64(); }

}  // namespace pico_bno085::hal
