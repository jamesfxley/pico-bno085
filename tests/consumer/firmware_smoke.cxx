// SPDX-License-Identifier: MIT
// AR-5: compile and link the installed SDK-backed HAL; never run this image.
#include "pico_bno085/hal/i2c_hal_transport.hpp"

int main() {
    pico_bno085::hal::I2cHalTransport transport(
        pico_bno085::hal::I2cHalTransport::Config{});
    return transport.data_ready() ? 0 : 1;
}
