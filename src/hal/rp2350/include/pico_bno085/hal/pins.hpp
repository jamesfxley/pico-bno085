// SPDX-License-Identifier: MIT
#pragma once

// GPIO assignments for a BNO085 wired to a Raspberry Pi Pico 2 (RP2350). One
// wiring supports both I2C and SPI: the protocol-select pins (PS0/PS1) are
// driven by the Pico before NRST is released, so the bus is chosen in firmware
// rather than by strapping. See docs/usage-guide.md for the wiring table.
//
// Firmware-only: this header names SDK GPIO numbers and is not built on host.

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

// I2C bus. The RP2350 function mux fixes which peripheral a pad can reach
// (RP2350 datasheet, Table 677, F3): the peripheral is (GPIO / 2) % 2, SDA is
// on even GPIOs and SCL on odd ones. So i2c0 takes SDA on GPIOs congruent to 0
// (mod 4) and SCL on 1, and i2c1 takes SDA on 2 and SCL on 3.
// I2C_INSTANCE_INDEX selects the peripheral and is bound to the pin numbers by
// static_assert, so the pair cannot drift out of the mux.
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
// The hub's SCL limit (1000-3927 §6.5.1); I2cHalTransport::init() refuses more.
inline constexpr std::uint32_t I2C_MAX_BAUD_HZ = 400000;

// SPI0 bus (mode 3, <= 3 MHz). The mux (RP2350 datasheet, Table 677, F1) puts
// the peripheral at (GPIO / 8) % 2 and the role at GPIO % 4: RX (MISO) 0, CSn
// 1, SCK 2, TX (MOSI) 3. CSN is driven as a plain GPIO, so only the others are
// checked. SpiHalTransport::Config defaults to SPI0, so the checks use index 0.
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
// The hub's SPI clock limit (1000-3927 §6.5.2); SpiHalTransport::init()
// refuses more.
inline constexpr std::uint32_t SPI_MAX_BAUD_HZ = 3000000;

// SA0/H_MOSI is a single hub pin: the I2C address bit in I2C mode, SPI data-in
// in SPI mode. One GPIO therefore serves both, driven as a plain output strap
// by ControlPins in I2C mode and owned by the SPI transport otherwise, so the
// shared net never has two drivers.
inline constexpr std::uint32_t PIN_SA0 = PIN_SPI_MOSI;

}  // namespace pico_bno085::hal
