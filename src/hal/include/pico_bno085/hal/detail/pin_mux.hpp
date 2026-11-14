// SPDX-License-Identifier: MIT
#pragma once

/// \file pin_mux.hpp
/// RP2350 pin-mux predicates (datasheet Table 677, F1 and F3).
/// GPIOs 30 to 47 exist only on the 80-pin package.

#include <cstdint>

namespace pico_bno085::hal::detail {

/// GPIO bank and peripheral instance limits.
struct MuxBounds {
    std::uint32_t gpio_count = 0;     ///< GPIOs in bank 0 (30 or 48).
    std::uint32_t i2c_instances = 2;  ///< I2C peripherals.
    std::uint32_t spi_instances = 2;  ///< SPI peripherals.
};

/// Bounds for the 30-GPIO package, used to check default pins.
inline constexpr MuxBounds BOUNDS_30_GPIOS{.gpio_count = 30};

/// Whether `gpio` is a GPIO of bank 0.
[[nodiscard]] constexpr bool in_bank(std::uint32_t gpio,
                                     const MuxBounds& bounds) {
    return gpio < bounds.gpio_count;
}

/// Whether `gpio` can carry SDA of I2C peripheral `instance`.
[[nodiscard]] constexpr bool i2c_sda(std::uint32_t gpio, std::uint32_t instance,
                                     const MuxBounds& bounds) {
    return in_bank(gpio, bounds) && instance < bounds.i2c_instances &&
           (gpio / 2) % 2 == instance && gpio % 2 == 0;
}

/// Whether `gpio` can carry SCL of I2C peripheral `instance`.
[[nodiscard]] constexpr bool i2c_scl(std::uint32_t gpio, std::uint32_t instance,
                                     const MuxBounds& bounds) {
    return in_bank(gpio, bounds) && instance < bounds.i2c_instances &&
           (gpio / 2) % 2 == instance && gpio % 2 == 1;
}

/// Whether `gpio` can carry RX (MISO) of SPI peripheral `instance`.
[[nodiscard]] constexpr bool spi_rx(std::uint32_t gpio, std::uint32_t instance,
                                    const MuxBounds& bounds) {
    return in_bank(gpio, bounds) && instance < bounds.spi_instances &&
           (gpio / 8) % 2 == instance && gpio % 4 == 0;
}

/// Whether `gpio` can carry SCK of SPI peripheral `instance`.
[[nodiscard]] constexpr bool spi_sck(std::uint32_t gpio, std::uint32_t instance,
                                     const MuxBounds& bounds) {
    return in_bank(gpio, bounds) && instance < bounds.spi_instances &&
           (gpio / 8) % 2 == instance && gpio % 4 == 2;
}

/// Whether `gpio` can carry TX (MOSI) of SPI peripheral `instance`.
[[nodiscard]] constexpr bool spi_tx(std::uint32_t gpio, std::uint32_t instance,
                                    const MuxBounds& bounds) {
    return in_bank(gpio, bounds) && instance < bounds.spi_instances &&
           (gpio / 8) % 2 == instance && gpio % 4 == 3;
}

}  // namespace pico_bno085::hal::detail
