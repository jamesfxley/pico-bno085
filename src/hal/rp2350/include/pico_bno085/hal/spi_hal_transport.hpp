// SPDX-License-Identifier: MIT
#pragma once

/// \file spi_hal_transport.hpp
/// IHalTransport over the RP2350 SPI peripheral (mode 3, <= 3 MHz). Owns the
/// strap/reset lines (ControlPins), the HINTN latch (HintnLatch), the CSN line,
/// and the host-write WAKE handshake (HR-4): drive PS0/WAKE low, wait for the
/// hub to assert HINTN, assert CSN, clock the frame. Reads are HINTN-gated
/// (HR-1) and stream header-then-body within one CSN assertion.
///
/// Firmware-only: it links the Pico SDK SPI/GPIO drivers.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "hardware/spi.h"
#include "pico_bno085/hal/control_pins.hpp"
#include "pico_bno085/hal/pins.hpp"
#include "pico_bno085/hal/transport_core.hpp"
#include "pico_sh2/sh2/hal_transport.hpp"
#include "pico_sh2/sh2/profile.hpp"
#include "pico_sh2/util/result.hpp"

namespace pico_bno085::hal {

/// Ceiling on the WAKE -> HINTN handshake. The datasheet gives twk, "BNO08X
/// wakeup from wake signal assert", as 150 µs typical (Figure 6-12) and
/// specifies no maximum, so this is a wedge guard with margin rather than a
/// derived bound: a write that waits this long has lost the hub, and reports
/// TIMEOUT rather than blocking service() further.
inline constexpr std::uint32_t SPI_WAKE_TIMEOUT_US = 3'000;

/// The SH-2 transport seam driven over SPI.
class SpiHalTransport final : public pico_sh2::IHalTransport {
   public:
    /// Board wiring and bus parameters. Defaults are the pins in pins.hpp.
    struct Config {
        /// Strap, reset and HINTN wiring.
        ControlConfig control{};
        spi_inst_t* instance = spi0;            ///< SDK SPI instance.
        std::uint32_t miso_pin = PIN_SPI_MISO;  ///< MISO GPIO.
        std::uint32_t csn_pin = PIN_SPI_CSN;    ///< Active-low chip select.
        std::uint32_t sck_pin = PIN_SPI_SCK;    ///< Clock GPIO.
        std::uint32_t mosi_pin = PIN_SPI_MOSI;  ///< MOSI GPIO (SA0's pin).
        std::uint32_t baud_hz =
            SPI_MAX_BAUD_HZ;  ///< Bus clock; init() refuses more.
        /// WAKE handshake ceiling; see SPI_WAKE_TIMEOUT_US.
        std::uint32_t wake_timeout_us = SPI_WAKE_TIMEOUT_US;
    };

    /// \param config board wiring and bus parameters.
    explicit SpiHalTransport(const Config& config)
        : config_(config),
          core_(config.control, Bus::SPI, /*sa0_high=*/false) {}

    /// Configure the control lines (hub held in reset), the SPI peripheral,
    /// CSN, and the HINTN interrupt. Call before Bno085::begin().
    ///
    /// \return INVALID_ARGUMENT, before any pin is touched, if `baud_hz` is
    ///         above the hub's limit, or once the peripheral is released again
    ///         if the rate the SDK actually sets is (HR-4); otherwise failure
    ///         only if the interrupt dispatch table is full, which is a wiring
    ///         mistake rather than a runtime condition.
    pico_sh2::Result<void> init();

    /// Whether a duplex cargo is stashed or HINTN is asserted (HR-1).
    bool data_ready() override {
        return pending_length_ != 0 || core_.data_ready();
    }

    /// Transfer, fault and latency counters for this transport.
    [[nodiscard]] TransportCounters counters() const {
        return core_.counters();
    }

    /// Start a fresh service-latency measurement window (HR-5).
    void clear_service_latency_peak() { core_.clear_service_latency_peak(); }

    /// Return a retained duplex transfer first, without bus activity; otherwise
    /// stream header and body within one CSN assertion.
    ///
    /// \param dst staging span for the transfer; at least
    ///        `pico_sh2::shtp::HEADER_SIZE` bytes, which the session's
    ///        profile-sized RX staging always satisfies.
    /// \return the number of bytes staged and the HINTN edge the read
    ///         answers, taken when its bus read or preceding duplex write
    ///         clears the latch (AR-2), or INVALID_ARGUMENT for a span shorter
    ///         than a header. An empty or absent hub reports success with its
    ///         header rather than an error, because the session's drain loop
    ///         faults on any read error.
    pico_sh2::Result<pico_sh2::ReceivedTransfer> read_transfer(
        std::span<std::uint8_t> dst) override;

    /// Send one frame with the WAKE handshake, retaining coincident RX (HR-4).
    ///
    /// \param frame the bytes to send.
    /// \return TIMEOUT if WAKE was not answered, INVALID_ARGUMENT for a short
    ///         or over-staging-capacity frame, or BUSY until a preceding duplex
    ///         cargo has been read.
    pico_sh2::Result<void> write_transfer(
        std::span<const std::uint8_t> frame) override;

    /// Microseconds since boot, from the SDK timer.
    std::uint64_t now_us() override { return TransportCore::now_us(); }

    /// Drive NRST, active low.
    ///
    /// \param asserted true to hold the hub in reset.
    void set_reset(bool asserted) override {
        if (asserted) {
            pending_length_ = 0;
        }
        core_.set_reset(asserted);
    }

    /// Release the HINTN interrupt and return the bus pads to SIO inputs, so
    /// another transport can claim them. The hub is left as it stands.
    void end();

   private:
    // Drive WAKE low and wait for the hub to assert HINTN, up to the configured
    // deadline. Returns false on timeout (WAKE left released).
    bool wake_and_wait();

    std::array<std::uint8_t, pico_sh2::ActiveProfile::MAX_RX_TRANSFER_LEN>
        pending_{};
    std::size_t pending_length_ = 0;
    std::uint64_t pending_edge_us_ = 0;
    Config config_;
    TransportCore core_;
};

}  // namespace pico_bno085::hal
