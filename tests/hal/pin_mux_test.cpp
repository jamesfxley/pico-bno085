// SPDX-License-Identifier: MIT
// The pin-mux rules the transports' init() refuses a pin by. The oracle is
// transcribed from the RP2350 datasheet, Table 677 (GPIO Bank 0 Functions),
// columns F1 (SPI) and F3 (I2C), one row per GPIO, so a formula error in the
// header fails here rather than agreeing with itself.
#include "pico_bno085/hal/detail/pin_mux.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

using pico_bno085::hal::detail::BOUNDS_30_GPIOS;
using pico_bno085::hal::detail::i2c_scl;
using pico_bno085::hal::detail::i2c_sda;
using pico_bno085::hal::detail::in_bank;
using pico_bno085::hal::detail::MuxBounds;
using pico_bno085::hal::detail::spi_rx;
using pico_bno085::hal::detail::spi_sck;
using pico_bno085::hal::detail::spi_tx;

constexpr std::uint32_t ROWS = 48;
constexpr MuxBounds FULL_BANK{.gpio_count = ROWS};
constexpr MuxBounds SMALL_BANK{.gpio_count = 30};

// Table 677, F1 column, GPIO 0 to 47, as the datasheet prints it.
// clang-format off
constexpr std::array<std::string_view, ROWS> F1_SPI{{
    "SPI0 RX", "SPI0 CSn", "SPI0 SCK", "SPI0 TX",  // 0-3
    "SPI0 RX", "SPI0 CSn", "SPI0 SCK", "SPI0 TX",  // 4-7
    "SPI1 RX", "SPI1 CSn", "SPI1 SCK", "SPI1 TX",  // 8-11
    "SPI1 RX", "SPI1 CSn", "SPI1 SCK", "SPI1 TX",  // 12-15
    "SPI0 RX", "SPI0 CSn", "SPI0 SCK", "SPI0 TX",  // 16-19
    "SPI0 RX", "SPI0 CSn", "SPI0 SCK", "SPI0 TX",  // 20-23
    "SPI1 RX", "SPI1 CSn", "SPI1 SCK", "SPI1 TX",  // 24-27
    "SPI1 RX", "SPI1 CSn", "SPI1 SCK", "SPI1 TX",  // 28-31
    "SPI0 RX", "SPI0 CSn", "SPI0 SCK", "SPI0 TX",  // 32-35
    "SPI0 RX", "SPI0 CSn", "SPI0 SCK", "SPI0 TX",  // 36-39
    "SPI1 RX", "SPI1 CSn", "SPI1 SCK", "SPI1 TX",  // 40-43
    "SPI1 RX", "SPI1 CSn", "SPI1 SCK", "SPI1 TX",  // 44-47
}};

// Table 677, F3 column, GPIO 0 to 47, as the datasheet prints it.
constexpr std::array<std::string_view, ROWS> F3_I2C{{
    "I2C0 SDA", "I2C0 SCL", "I2C1 SDA", "I2C1 SCL",  // 0-3
    "I2C0 SDA", "I2C0 SCL", "I2C1 SDA", "I2C1 SCL",  // 4-7
    "I2C0 SDA", "I2C0 SCL", "I2C1 SDA", "I2C1 SCL",  // 8-11
    "I2C0 SDA", "I2C0 SCL", "I2C1 SDA", "I2C1 SCL",  // 12-15
    "I2C0 SDA", "I2C0 SCL", "I2C1 SDA", "I2C1 SCL",  // 16-19
    "I2C0 SDA", "I2C0 SCL", "I2C1 SDA", "I2C1 SCL",  // 20-23
    "I2C0 SDA", "I2C0 SCL", "I2C1 SDA", "I2C1 SCL",  // 24-27
    "I2C0 SDA", "I2C0 SCL", "I2C1 SDA", "I2C1 SCL",  // 28-31
    "I2C0 SDA", "I2C0 SCL", "I2C1 SDA", "I2C1 SCL",  // 32-35
    "I2C0 SDA", "I2C0 SCL", "I2C1 SDA", "I2C1 SCL",  // 36-39
    "I2C0 SDA", "I2C0 SCL", "I2C1 SDA", "I2C1 SCL",  // 40-43
    "I2C0 SDA", "I2C0 SCL", "I2C1 SDA", "I2C1 SCL",  // 44-47
}};
// clang-format on

// Whether Table 677 puts `role` of `bus` (for example "SPI", "RX") on `gpio`.
bool table_has(const std::array<std::string_view, ROWS>& column,
               std::uint32_t gpio, std::string_view bus, std::uint32_t instance,
               std::string_view role) {
    const std::string wanted =
        std::string(bus) + std::to_string(instance) + " " + std::string(role);
    return column.at(gpio) == wanted;
}

struct RolePredicate {
    std::string_view role;
    bool (*accepts)(std::uint32_t, std::uint32_t, const MuxBounds&);
};

constexpr std::array<RolePredicate, 3> SPI_ROLES{{
    {.role = "RX", .accepts = spi_rx},
    {.role = "SCK", .accepts = spi_sck},
    {.role = "TX", .accepts = spi_tx},
}};

constexpr std::array<RolePredicate, 2> I2C_ROLES{{
    {.role = "SDA", .accepts = i2c_sda},
    {.role = "SCL", .accepts = i2c_scl},
}};

TEST(PinMux, SpiRoleAndInstanceMatchTable677ForEveryGpio) {
    for (std::uint32_t gpio = 0; gpio < ROWS; ++gpio) {
        for (const RolePredicate& role : SPI_ROLES) {
            for (std::uint32_t instance = 0; instance < 2; ++instance) {
                EXPECT_EQ(role.accepts(gpio, instance, FULL_BANK),
                          table_has(F1_SPI, gpio, "SPI", instance, role.role))
                    << "gpio " << gpio << " " << role.role << " instance "
                    << instance;
            }
        }
    }
}

TEST(PinMux, I2cRoleAndInstanceMatchTable677ForEveryGpio) {
    for (std::uint32_t gpio = 0; gpio < ROWS; ++gpio) {
        for (const RolePredicate& role : I2C_ROLES) {
            for (std::uint32_t instance = 0; instance < 2; ++instance) {
                EXPECT_EQ(role.accepts(gpio, instance, FULL_BANK),
                          table_has(F3_I2C, gpio, "I2C", instance, role.role))
                    << "gpio " << gpio << " " << role.role << " instance "
                    << instance;
            }
        }
    }
}

TEST(PinMux, ASmallBankRefusesEveryGpioFromThirty) {
    for (std::uint32_t gpio = 0; gpio < ROWS; ++gpio) {
        const bool present = gpio < 30;
        EXPECT_EQ(in_bank(gpio, SMALL_BANK), present) << "gpio " << gpio;
        EXPECT_TRUE(in_bank(gpio, FULL_BANK)) << "gpio " << gpio;
        for (std::uint32_t instance = 0; instance < 2; ++instance) {
            for (const RolePredicate& role : SPI_ROLES) {
                EXPECT_EQ(role.accepts(gpio, instance, SMALL_BANK),
                          present && role.accepts(gpio, instance, FULL_BANK))
                    << "gpio " << gpio << " " << role.role;
            }
            for (const RolePredicate& role : I2C_ROLES) {
                EXPECT_EQ(role.accepts(gpio, instance, SMALL_BANK),
                          present && role.accepts(gpio, instance, FULL_BANK))
                    << "gpio " << gpio << " " << role.role;
            }
        }
    }
}

TEST(PinMux, EveryGpioBeyondTheBankIsRefusedWhateverTheRole) {
    for (std::uint32_t gpio = 48; gpio < 56; ++gpio) {
        EXPECT_FALSE(in_bank(gpio, FULL_BANK));
        EXPECT_FALSE(i2c_sda(gpio, 0, FULL_BANK));
        EXPECT_FALSE(i2c_scl(gpio, 1, FULL_BANK));
        EXPECT_FALSE(spi_rx(gpio, 0, FULL_BANK));
        EXPECT_FALSE(spi_sck(gpio, 1, FULL_BANK));
        EXPECT_FALSE(spi_tx(gpio, 1, FULL_BANK));
    }
}

TEST(PinMux, AnInstanceTheChipLacksIsRefused) {
    EXPECT_FALSE(i2c_sda(2, 2, FULL_BANK));
    EXPECT_FALSE(i2c_scl(3, 3, FULL_BANK));
    EXPECT_FALSE(spi_rx(0, 2, FULL_BANK));
    EXPECT_FALSE(spi_sck(2, 2, FULL_BANK));
    EXPECT_FALSE(spi_tx(3, 2, FULL_BANK));
}

TEST(PinMux, TheDefaultPinsAreAcceptedForTheirInstances) {
    // The wiring pins.hpp defaults to: i2c1 on GP2/GP3, SPI0 on GP16/18/19.
    EXPECT_TRUE(i2c_sda(2, 1, BOUNDS_30_GPIOS));
    EXPECT_TRUE(i2c_scl(3, 1, BOUNDS_30_GPIOS));
    EXPECT_FALSE(i2c_sda(2, 0, BOUNDS_30_GPIOS));
    EXPECT_TRUE(spi_rx(16, 0, BOUNDS_30_GPIOS));
    EXPECT_TRUE(spi_sck(18, 0, BOUNDS_30_GPIOS));
    EXPECT_TRUE(spi_tx(19, 0, BOUNDS_30_GPIOS));
    EXPECT_FALSE(spi_tx(19, 1, BOUNDS_30_GPIOS));
}

}  // namespace
