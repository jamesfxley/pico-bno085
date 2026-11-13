// SPDX-License-Identifier: MIT
#pragma once

/// \file spi_hal_transport.hpp
/// SDK SPI transport with WAKE handshakes (HR-4) and HINTN-gated reads (HR-1).

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

/// WAKE-to-HINTN timeout in microseconds (1000-3927 Figure 6-12).
/// This is a timeout guard, not a datasheet maximum.
inline constexpr std::uint32_t SPI_WAKE_TIMEOUT_US = 3'000;

/// The SH-2 transport seam driven over SPI.
class SpiHalTransport final : public pico_sh2::IHalTransport {
   public:
    /// Board wiring and bus parameters. Defaults are the pins in pins.hpp.
    struct Config {
        /// Strap, reset and HINTN wiring.
        ControlConfig control{};
        /// SDK SPI instance.
        spi_inst_t* instance = spi0;
        /// MISO GPIO.
        std::uint32_t miso_pin = PIN_SPI_MISO;
        /// Active-low chip select.
        std::uint32_t csn_pin = PIN_SPI_CSN;
        /// Clock GPIO.
        std::uint32_t sck_pin = PIN_SPI_SCK;
        /// MOSI GPIO (SA0's pin).
        std::uint32_t mosi_pin = PIN_SPI_MOSI;
        /// Bus clock in hertz, bounded by the hub limit.
        std::uint32_t baud_hz = SPI_MAX_BAUD_HZ;
        /// WAKE handshake ceiling; see SPI_WAKE_TIMEOUT_US.
        std::uint32_t wake_timeout_us = SPI_WAKE_TIMEOUT_US;
    };

    /// \param config board wiring and bus parameters.
    explicit SpiHalTransport(const Config& config)
        : config_(config),
          core_(config.control, Bus::SPI, /*sa0_high=*/false) {}

    /// Initialise SPI and control pins before Bno085::begin().
    /// \return INVALID_ARGUMENT for excessive baud, unreachable MISO/SCK/MOSI
    /// (RP2350 Table 677) or out-of-bank pins, including CSN, before touching
    /// pins. An excessive actual SDK baud releases the peripheral and returns
    /// the same error. Otherwise only TABLE_FULL can fail (HR-4).
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

    /// Return staged duplex RX first, or read header and body under one CSN
    /// assertion.
    /// \param dst staging span, at least pico_sh2::shtp::HEADER_SIZE bytes.
    /// \return Staged length and edge taken when the read or duplex write
    /// cleared the latch (AR-2), or INVALID_ARGUMENT for a short span. Empty or
    /// absent hubs return their header successfully.
    pico_sh2::Result<pico_sh2::ReceivedTransfer> read_transfer(
        std::span<std::uint8_t> dst) override;

    /// Send with a WAKE handshake and retain coincident RX (HR-4).
    /// \return TIMEOUT if WAKE is unanswered, INVALID_ARGUMENT for a short or
    /// oversized frame, or BUSY until the previous duplex cargo is read.
    pico_sh2::Result<void> write_transfer(
        std::span<const std::uint8_t> frame) override;

    /// Microseconds since boot, from the SDK timer.
    std::uint64_t now_us() override { return TransportCore::now_us(); }

    /// Drive active-low NRST.
    /// \param asserted true to hold the hub in reset.
    void set_reset(bool asserted) override {
        if (asserted) {
            pending_length_ = 0;
        }
        core_.set_reset(asserted);
    }

    /// Release HINTN and return bus pads to SIO inputs. The hub state is
    /// unchanged.
    void end();

   private:
    // Assert WAKE until HINTN or timeout. Return false with WAKE released on
    // timeout.
    bool wake_and_wait();

    std::array<std::uint8_t, pico_sh2::ActiveProfile::MAX_RX_TRANSFER_LEN>
        pending_{};
    std::size_t pending_length_ = 0;
    std::uint64_t pending_edge_us_ = 0;
    Config config_;
    TransportCore core_;
};

}  // namespace pico_bno085::hal
