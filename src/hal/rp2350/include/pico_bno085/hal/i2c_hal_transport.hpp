// SPDX-License-Identifier: MIT
#pragma once

/// \file i2c_hal_transport.hpp
/// SDK I2C transport with HINTN-gated reads and transaction timeouts (HR-1,
/// HR-2).

#include <cstddef>
#include <cstdint>
#include <span>

#include "hardware/i2c.h"
#include "pico_bno085/hal/control_pins.hpp"
#include "pico_bno085/hal/pins.hpp"
#include "pico_bno085/hal/split_read.hpp"
#include "pico_bno085/hal/transport_core.hpp"
#include "pico_sh2/sh2/hal_transport.hpp"
#include "pico_sh2/util/result.hpp"

namespace pico_bno085::hal {

/// I2C transaction timeout in microseconds.
inline constexpr std::uint32_t I2C_TRANSFER_TIMEOUT_US = 30'000;

/// The SH-2 transport seam driven over I2C.
class I2cHalTransport final : public pico_sh2::IHalTransport {
   public:
    /// Board wiring and bus parameters. Defaults are the pins in pins.hpp.
    struct Config {
        /// Strap, reset and HINTN wiring.
        ControlConfig control{};
        /// SDK I2C instance to drive.
        i2c_inst_t* instance = I2C_INSTANCE_INDEX == 0 ? i2c0 : i2c1;
        /// 7-bit hub address, selected by the SA0 strap.
        std::uint8_t address = static_cast<std::uint8_t>(I2C_ADDRESS_SA0_LOW);
        /// SDA GPIO.
        std::uint32_t sda_pin = PIN_I2C_SDA;
        /// SCL GPIO.
        std::uint32_t scl_pin = PIN_I2C_SCL;
        /// Bus clock in hertz, bounded by the hub limit.
        std::uint32_t baud_hz = I2C_MAX_BAUD_HZ;
        /// Per-transaction ceiling; see I2C_TRANSFER_TIMEOUT_US.
        std::uint32_t transfer_timeout_us = I2C_TRANSFER_TIMEOUT_US;
    };

    /// \param config board wiring and bus parameters.
    explicit I2cHalTransport(const Config& config)
        : config_(config),
          core_(config.control, Bus::I2C,
                config.address == I2C_ADDRESS_SA0_HIGH) {}

    /// Initialise I2C and control pins before Bno085::begin().
    /// \return INVALID_ARGUMENT for excessive baud or unreachable SDA/SCL
    /// (RP2350 Table 677), or out-of-bank pins, before touching pins. An
    /// excessive actual SDK baud releases the peripheral and returns the same
    /// error. Otherwise only TABLE_FULL can fail (HR-4).
    pico_sh2::Result<void> init();

    /// Whether the hub has a transfer waiting: HINTN asserted (HR-1).
    bool data_ready() override { return core_.data_ready(); }

    /// Transfer, fault and latency counters for this transport.
    [[nodiscard]] TransportCounters counters() const {
        return core_.counters();
    }

    /// Start a fresh service-latency measurement window (HR-5).
    void clear_service_latency_peak() { core_.clear_service_latency_peak(); }

    /// Retrieve a transfer and undo the header peek's effects (HR-6).
    /// \param dst staging span, at least pico_sh2::shtp::HEADER_SIZE bytes.
    /// \return Staged length and edge taken when clearing the latch (AR-2), or
    /// INVALID_ARGUMENT for a short span. Empty or absent hubs return a
    /// successful empty transfer.
    pico_sh2::Result<pico_sh2::ReceivedTransfer> read_transfer(
        std::span<std::uint8_t> dst) override;

    /// Send one frame to the hub.
    pico_sh2::Result<void> write_transfer(
        std::span<const std::uint8_t> frame) override;

    /// Microseconds since boot, from the SDK timer.
    std::uint64_t now_us() override { return TransportCore::now_us(); }

    /// Drive active-low NRST.
    /// \param asserted true to hold the hub in reset.
    void set_reset(bool asserted) override { core_.set_reset(asserted); }

    /// Release HINTN and return bus pads to SIO inputs. The hub state is
    /// unchanged.
    void end();

   private:
    // Count an absent read, clear compensation state and stage an empty header.
    // Return the staged length.
    std::size_t note_absent(std::span<std::uint8_t> dst);

    Config config_;
    TransportCore core_;

    // Compensation for the header peek's continuation flag and sequence (HR-6).
    SplitReadCompensator split_read_;
};

}  // namespace pico_bno085::hal
