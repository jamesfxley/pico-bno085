// SPDX-License-Identifier: MIT
#pragma once

/// \file transport_core.hpp
/// Shared control pins, live HINTN readiness and edge timing (HR-1, AR-2).

#include <cstdint>

#include "pico_bno085/hal/control_pins.hpp"
#include "pico_bno085/hal/edge_latch.hpp"
#include "pico_bno085/hal/hintn_irq.hpp"
#include "pico_bno085/hal/pins.hpp"
#include "pico_sh2/util/result.hpp"

namespace pico_bno085::hal {

/// Control wiring shared by both bus configurations.
struct ControlConfig {
    std::uint32_t hintn_pin = PIN_HINTN;   ///< Active-low data-ready interrupt.
    std::uint32_t nrst_pin = PIN_NRST;     ///< Active-low reset.
    std::uint32_t ps0_pin = PIN_PS0_WAKE;  ///< Strap, then SPI WAKE.
    std::uint32_t ps1_pin = PIN_PS1;       ///< Protocol-select strap.
    std::uint32_t bootn_pin = PIN_BOOTN;   ///< Boot-mode strap.
    std::uint32_t sa0_pin = PIN_SA0;       ///< I2C address strap / SPI MOSI.
};

/// Bus-independent transfer counters.
struct TransportCounters {
    /// Reads without a latched edge. Their timestamps can use an earlier edge
    /// (HR-1).
    std::uint32_t level_fallback_reads = 0;
    /// Reads with an unacknowledged I2C address or a dead-bus SPI header
    /// (HR-2).
    std::uint32_t absent_reads = 0;
    /// Longest HINTN-to-read gap in microseconds, measured from the first
    /// unserviced edge (HR-5).
    std::uint32_t service_latency_max_us = 0;
};

/// The wiring, latch and clock both transports share.
class TransportCore {
   public:
    /// \param config board wiring.
    /// \param bus the selected bus.
    /// \param sa0_high I2C address strap level; ignored for SPI.
    TransportCore(const ControlConfig& config, Bus bus, bool sa0_high)
        : config_(config),
          control_({.nrst_pin = config.nrst_pin,
                    .ps0_pin = config.ps0_pin,
                    .ps1_pin = config.ps1_pin,
                    .bootn_pin = config.bootn_pin,
                    .sa0_pin = config.sa0_pin},
                   bus, sa0_high) {}

    /// \name Copy and move
    /// The interrupt table retains the HINTN latch's address.
    /// \{
    TransportCore(const TransportCore&) = delete;
    TransportCore& operator=(const TransportCore&) = delete;
    TransportCore(TransportCore&&) = delete;
    TransportCore& operator=(TransportCore&&) = delete;
    /// \}

    ~TransportCore() = default;

    /// Drive straps with NRST asserted (HR-3).
    /// The transport's init() configures its bus between begin_straps()
    /// and arm_interrupt().
    void begin_straps() { control_.init(); }

    /// Arm HINTN after bus configuration.
    /// \return INVALID_ARGUMENT if the HINTN pin is outside bank 0,
    /// TABLE_FULL if no dispatch slot is available.
    pico_sh2::Result<void> arm_interrupt() {
        return hintn_.begin(config_.hintn_pin);
    }

    /// Release the HINTN interrupt.
    void end() { hintn_.end(); }

    /// Read live HINTN for readiness (1000-3535 §2.6) (AR-2, HR-1).
    [[nodiscard]] bool data_ready() const { return hintn_.asserted(); }

    /// Clear the pending edge and measure latency from the first unserviced
    /// edge (HR-5).
    /// \return Whether the read consumed an edge, and its timestamp (HR-1,
    /// AR-2).
    ConsumedEdge consume() {
        const std::uint64_t edge = hintn_.pending_since_us();
        const ConsumedEdge consumed = hintn_.consume();
        if (consumed.had_edge) {
            const std::uint64_t now = now_us();
            if (now > edge) {
                const std::uint64_t gap = now - edge;
                if (gap > service_latency_max_us_) {
                    service_latency_max_us_ = static_cast<std::uint32_t>(gap);
                }
            }
        }
        return consumed;
    }

    /// Clear an assertion answered by a completed transfer (1000-3535 §2.6).
    /// A split read must account for its second assertion (HR-5, HR-6).
    /// A concurrent cargo still reaches the level fallback (HR-1).
    void discard_continuation_assertion() {
        hintn_.discard_continuation_assertion();
    }

    /// Record a read the hub did not answer.
    void note_absent_read() { ++absent_reads_; }

    /// Clear the latency peak before measuring steady-state service (HR-5).
    void clear_service_latency_peak() { service_latency_max_us_ = 0; }

    /// Transfer, fault and latency counters for this transport.
    [[nodiscard]] TransportCounters counters() const {
        return {.level_fallback_reads = hintn_.level_fallback_reads(),
                .absent_reads = absent_reads_,
                .service_latency_max_us = service_latency_max_us_};
    }

    /// Drive active-low NRST.
    /// \param asserted true to hold the hub in reset.
    void set_reset(bool asserted) { control_.set_reset(asserted); }

    /// PS0, repurposed by the hub as the active-low SPI WAKE line after reset.
    [[nodiscard]] std::uint32_t wake_pin() const { return control_.wake_pin(); }

    /// The board wiring this core was constructed with.
    [[nodiscard]] const ControlConfig& config() const { return config_; }

    /// Monotonic microseconds since boot; the session's whole time base.
    static std::uint64_t now_us();

   private:
    ControlConfig config_;
    ControlPins control_;
    HintnLatch hintn_;
    std::uint32_t absent_reads_ = 0;
    std::uint32_t service_latency_max_us_ = 0;
};

}  // namespace pico_bno085::hal
