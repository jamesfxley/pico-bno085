// SPDX-License-Identifier: MIT
#pragma once

// Reference GPIO wiring for both buses. See docs/usage-guide.md.

#include <cstdint>

#include "pico_bno085/hal/detail/pin_mux.hpp"

namespace pico_bno085::hal {

// Shared control lines (both buses).
inline constexpr std::uint32_t PIN_HINTN =
    6;  // host interrupt, active low, pull-up, falling IRQ
inline constexpr std::uint32_t PIN_NRST = 7;  // hub reset, active low
inline constexpr std::uint32_t PIN_PS0_WAKE =
    8;  // protocol select bit 0; SPI WAKE after reset
inline constexpr std::uint32_t PIN_PS1 =
    9;  // protocol select bit 1 (0=I2C, 1=SPI)
inline constexpr std::uint32_t PIN_BOOTN =
    10;  // bootloader select, held high (DFU out of scope)

// Default I2C pins, checked against RP2350 Table 677, F3.
inline constexpr std::uint32_t I2C_INSTANCE_INDEX = 1;
inline constexpr std::uint32_t PIN_I2C_SDA = 2;
inline constexpr std::uint32_t PIN_I2C_SCL = 3;

static_assert(I2C_INSTANCE_INDEX <= 1, "RP2350 has two I2C peripherals");
static_assert(detail::i2c_sda(PIN_I2C_SDA, I2C_INSTANCE_INDEX,
                              detail::BOUNDS_30_GPIOS),
              "PIN_I2C_SDA cannot reach the selected I2C peripheral");
static_assert(detail::i2c_scl(PIN_I2C_SCL, I2C_INSTANCE_INDEX,
                              detail::BOUNDS_30_GPIOS),
              "PIN_I2C_SCL cannot reach the selected I2C peripheral");

inline constexpr std::uint32_t I2C_ADDRESS_SA0_LOW = 0x4A;
inline constexpr std::uint32_t I2C_ADDRESS_SA0_HIGH = 0x4B;
// Hub SCL limit in hertz (1000-3927 §6.5.1).
inline constexpr std::uint32_t I2C_MAX_BAUD_HZ = 400000;

// Default SPI0 pins, checked against RP2350 Table 677, F1.
// CSN is driven as a plain GPIO.
inline constexpr std::uint32_t PIN_SPI_MISO = 16;
inline constexpr std::uint32_t PIN_SPI_CSN = 17;  // driven as a plain GPIO
inline constexpr std::uint32_t PIN_SPI_SCK = 18;
inline constexpr std::uint32_t PIN_SPI_MOSI = 19;
static_assert(detail::spi_rx(PIN_SPI_MISO, 0, detail::BOUNDS_30_GPIOS),
              "PIN_SPI_MISO cannot reach SPI0");
static_assert(detail::spi_sck(PIN_SPI_SCK, 0, detail::BOUNDS_30_GPIOS),
              "PIN_SPI_SCK cannot reach SPI0");
static_assert(detail::spi_tx(PIN_SPI_MOSI, 0, detail::BOUNDS_30_GPIOS),
              "PIN_SPI_MOSI cannot reach SPI0");
// Hub SPI clock limit in hertz (1000-3927 §6.5.2).
inline constexpr std::uint32_t SPI_MAX_BAUD_HZ = 3000000;

// SA0/H_MOSI is shared: ControlPins drives the I2C address strap,
// and the SPI transport owns MOSI.
inline constexpr std::uint32_t PIN_SA0 = PIN_SPI_MOSI;

}  // namespace pico_bno085::hal
