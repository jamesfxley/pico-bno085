// SPDX-License-Identifier: MIT
#pragma once

/// \file pin_mux.hpp
/// Which GPIOs the RP2350 function mux lets an I2C or SPI peripheral reach, as
/// predicates over plain integers, so the rules are tested on the host. The
/// transports' init() refuses a pin these reject, and pins.hpp binds its
/// defaults to them at compile time.
///
/// The rules are those of the RP2350 datasheet, Table 677 (GPIO Bank 0
/// Functions), columns F1 (SPI) and F3 (I2C):
///  - I2C instance = (gpio / 2) % 2; SDA on even GPIOs, SCL on odd ones.
///  - SPI instance = (gpio / 8) % 2; by gpio % 4, RX (MISO) 0, CSn 1, SCK 2,
///    TX (MOSI) 3.
///
/// Every predicate also bounds the GPIO by the bank's size, because GPIOs 30
/// to 47 exist on the 80-pin package only.

#include <cstdint>

namespace pico_bno085::hal::detail {

/// The number of GPIOs in bank 0 and the number of instances of a bus, which
/// bound every pin check.
struct MuxBounds {
    std::uint32_t gpio_count = 0;     ///< GPIOs in bank 0 (30 or 48).
    std::uint32_t i2c_instances = 2;  ///< I2C peripherals.
    std::uint32_t spi_instances = 2;  ///< SPI peripherals.
};

/// The bounds of the smaller package, whose 30 GPIOs every RP2350 has; the
/// compile-time checks of the default pins use them.
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
