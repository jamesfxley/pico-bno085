// SPDX-License-Identifier: MIT
#pragma once

/// \file control_pins.hpp
/// Drives the BNO085 strap and reset lines shared by both transports. PS0/PS1
/// select the bus, BOOTN the boot mode, and SA0 the I2C address; every strap
/// the selected bus needs must be stable before NRST is released (HR-3). NRST
/// itself is exposed through set_reset() so the SH-2 session can sequence
/// assert -> settle -> release.
///
/// SA0 shares the hub's pin with SPI data-in (SA0/H_MOSI), so it is driven only
/// in I2C mode; in SPI mode that pin is the SPI transport's MOSI.
///
/// After reset, PS0 is repurposed by the hub as the active-low SPI WAKE line;
/// the SPI transport owns it from then on. Its strap idle level (high for SPI)
/// equals the WAKE idle level, so releasing reset does not glitch it.
///
/// Firmware-only: it drives SDK GPIO.

#include <cstdint>

#include "pico_bno085/hal/pins.hpp"

namespace pico_bno085::hal {

/// Which bus the protocol straps select at the reset edge.
enum class Bus : std::uint8_t {
    I2C,  ///< PS1/PS0 strapped for I2C; SA0 selects the address.
    SPI,  ///< PS1/PS0 strapped for SPI; SA0's pin becomes MOSI.
};

/// Owns the strap and reset lines, and drives them for one selected bus.
class ControlPins {
   public:
    /// Board wiring. Defaults are the pins in pins.hpp.
    struct Config {
        std::uint32_t nrst_pin = PIN_NRST;     ///< Active-low reset.
        std::uint32_t ps0_pin = PIN_PS0_WAKE;  ///< Strap, then SPI WAKE.
        std::uint32_t ps1_pin = PIN_PS1;       ///< Protocol-select strap.
        std::uint32_t bootn_pin = PIN_BOOTN;   ///< Boot-mode strap.
        std::uint32_t sa0_pin = PIN_SA0;       ///< I2C address strap / MOSI.
    };

    /// \param config the board wiring.
    /// \param bus the bus to strap for.
    /// \param sa0_high the I2C address strap level; ignored in SPI mode, where
    ///        that pin belongs to the SPI transport.
    ControlPins(const Config& config, Bus bus, bool sa0_high)
        : config_(config), bus_(bus), sa0_high_(sa0_high) {}

    /// Configure every control pin as an output and drive the straps for the
    /// selected bus with NRST asserted (the hub held in reset). Call before the
    /// session releases reset so the straps are latched at the reset edge.
    void init();

    /// Drive NRST. It is active low, so set_reset(true) drives the line low.
    /// The session sequences the assert/hold/release; init() leaves the line
    /// asserted.
    ///
    /// \param asserted true to hold the hub in reset.
    void set_reset(bool asserted) const;

    /// PS0, repurposed as the active-low SPI WAKE line after reset.
    [[nodiscard]] std::uint32_t wake_pin() const { return config_.ps0_pin; }

   private:
    Config config_;
    Bus bus_;
    bool sa0_high_;
};

}  // namespace pico_bno085::hal
