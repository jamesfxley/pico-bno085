// SPDX-License-Identifier: MIT
#pragma once

/// \file control_pins.hpp
/// Shared strap and reset GPIOs. Straps needed by the selected bus must be
/// stable before NRST is released (HR-3).
/// After reset PS0 is the SPI transport's active-low WAKE line. Its SPI strap
/// level equals WAKE's idle level, so releasing reset does not glitch it.

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

    /// \param config board wiring.
    /// \param bus the selected bus.
    /// \param sa0_high I2C address strap level; ignored for SPI, which owns
    /// MOSI.
    ControlPins(const Config& config, Bus bus, bool sa0_high)
        : config_(config), bus_(bus), sa0_high_(sa0_high) {}

    /// Drive straps with NRST asserted before the session releases reset.
    void init();

    /// Drive active-low NRST. init() leaves reset asserted.
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
