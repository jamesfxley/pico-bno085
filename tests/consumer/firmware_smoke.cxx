// SPDX-License-Identifier: MIT
// Compile and link the installed SDK HAL without running it (AR-5).
#include "pico_bno085/hal/i2c_hal_transport.hpp"

int main() {
    pico_bno085::hal::I2cHalTransport transport(
        pico_bno085::hal::I2cHalTransport::Config{});
    return transport.data_ready() ? 0 : 1;
}
