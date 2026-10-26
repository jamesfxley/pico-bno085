// SPDX-License-Identifier: MIT
#pragma once

/// \file i2c_hal_transport.hpp
/// IHalTransport over the RP2350 I2C peripheral. Owns the strap/reset lines
/// (ControlPins), the HINTN latch (HintnLatch), and the HINTN-gated read
/// sequencing. A read begins only while HINTN is asserted (HR-1) and
/// every transaction is timeout-bounded (HR-2), so a clock-stretching BNO085 or
/// a zero-length BNO086 poll can never wedge service().
///
/// Firmware-only: it links the Pico SDK I2C/GPIO drivers.

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

/// Per-transaction I2C ceiling. Bounds a bus fault or a wedged clock stretch;
/// it is not the HINTN-service latency budget. Covers a full large-profile
/// transfer at the minimum baud plus stretch margin.
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
        std::uint32_t sda_pin = PIN_I2C_SDA;  ///< SDA GPIO.
        std::uint32_t scl_pin = PIN_I2C_SCL;  ///< SCL GPIO.
        std::uint32_t baud_hz =
            I2C_MAX_BAUD_HZ;  ///< Bus clock; init() refuses more.
        /// Per-transaction ceiling; see I2C_TRANSFER_TIMEOUT_US.
        std::uint32_t transfer_timeout_us = I2C_TRANSFER_TIMEOUT_US;
    };

    /// \param config board wiring and bus parameters.
    explicit I2cHalTransport(const Config& config)
        : config_(config),
          core_(config.control, Bus::I2C,
                config.address == I2C_ADDRESS_SA0_HIGH) {}

    /// Configure the control lines (hub held in reset), the I2C peripheral, and
    /// the HINTN interrupt. Call before Bno085::begin().
    ///
    /// \return INVALID_ARGUMENT, before any pin is touched, if `baud_hz` is
    ///         above the hub's limit, or once the peripheral is released again
    ///         if the rate the SDK actually sets is (HR-4); otherwise failure
    ///         only if the interrupt dispatch table is full, which is a wiring
    ///         mistake rather than a runtime condition.
    pico_sh2::Result<void> init();

    /// Whether the hub has a transfer waiting: HINTN asserted (HR-1).
    bool data_ready() override { return core_.data_ready(); }

    /// Transfer, fault and latency counters for this transport.
    [[nodiscard]] TransportCounters counters() const {
        return core_.counters();
    }

    /// Start a fresh service-latency measurement window (HR-5).
    void clear_service_latency_peak() { core_.clear_service_latency_peak(); }

    /// Retrieve one transfer, undoing the traces of the split read (HR-6).
    ///
    /// \param dst staging span for the transfer; at least
    ///        `pico_sh2::shtp::HEADER_SIZE` bytes, which the session's
    ///        profile-sized RX staging always satisfies.
    /// \return the number of bytes staged and the HINTN edge the read
    ///         answers, taken as the read clears the latch (AR-2), or
    ///         INVALID_ARGUMENT for a span
    ///         shorter than a header. An empty or absent hub reports success
    ///         with a zero-length header rather than an error, because the
    ///         session's drain loop faults on any read error.
    pico_sh2::Result<pico_sh2::ReceivedTransfer> read_transfer(
        std::span<std::uint8_t> dst) override;

    /// Send one frame to the hub.
    ///
    /// \param frame the bytes to send.
    pico_sh2::Result<void> write_transfer(
        std::span<const std::uint8_t> frame) override;

    /// Microseconds since boot, from the SDK timer.
    std::uint64_t now_us() override { return TransportCore::now_us(); }

    /// Drive NRST, active low.
    ///
    /// \param asserted true to hold the hub in reset.
    void set_reset(bool asserted) override { core_.set_reset(asserted); }

    /// Release the HINTN interrupt and return the bus pads to SIO inputs, so
    /// another transport can claim them. The hub is left as it stands.
    void end();

   private:
    // Record a read the hub did not acknowledge and stage the zero-length
    // header reported in its place, clearing the split-read state the hub
    // abandons while it is away. Returns the transfer length to report.
    std::size_t note_absent(std::span<std::uint8_t> dst);

    Config config_;
    TransportCore core_;

    // Undoes the continuation flag and the extra sequence number the hub's
    // answer to a split read carries (HR-6).
    SplitReadCompensator split_read_;
};

}  // namespace pico_bno085::hal
