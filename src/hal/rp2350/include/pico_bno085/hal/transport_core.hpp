// SPDX-License-Identifier: MIT
#pragma once

/// \file transport_core.hpp
/// The board wiring both transports own regardless of which bus they drive: the
/// strap and reset lines, the HINTN latch, and the monotonic clock.
///
/// I2C and SPI differ in how they move bytes, not in how the hub is wired or
/// how it signals. Holding that in one place keeps the two transports from
/// drifting apart in the parts that are not bus-specific, and gives a single
/// answer to "is there data pending" (HR-1) and "when did the edge arrive"
/// (AR-2).
///
/// Firmware-only: it drives SDK GPIO and the SDK timer.

#include <cstdint>

#include "pico_bno085/hal/control_pins.hpp"
#include "pico_bno085/hal/edge_latch.hpp"
#include "pico_bno085/hal/hintn_irq.hpp"
#include "pico_bno085/hal/pins.hpp"
#include "pico_sh2/util/result.hpp"

namespace pico_bno085::hal {

/// The pins every BNO085 wiring has, whichever bus carries the cargoes. A
/// consumer on different wiring overrides this one struct rather than restating
/// the same fields in each transport's own config.
struct ControlConfig {
    std::uint32_t hintn_pin = PIN_HINTN;   ///< Active-low data-ready interrupt.
    std::uint32_t nrst_pin = PIN_NRST;     ///< Active-low reset.
    std::uint32_t ps0_pin = PIN_PS0_WAKE;  ///< Strap, then SPI WAKE.
    std::uint32_t ps1_pin = PIN_PS1;       ///< Protocol-select strap.
    std::uint32_t bootn_pin = PIN_BOOTN;   ///< Boot-mode strap.
    std::uint32_t sa0_pin = PIN_SA0;       ///< I2C address strap / SPI MOSI.
};

/// What a transport can report about the wire beneath the session. Both buses
/// answer the same questions, so a consumer reading them needs no knowledge of
/// which one it is talking to.
struct TransportCounters {
    /// Reads served by the level fallback with no latched edge behind them, so
    /// the events they carry are anchored to an earlier edge (HR-1).
    std::uint32_t level_fallback_reads = 0;
    /// Reads the hub did not answer: an unacknowledged address on I2C, the
    /// reserved dead-bus header on SPI. Each one is the hub off the bus, which
    /// is what a reset looks like from here (HR-2).
    std::uint32_t absent_reads = 0;
    /// Longest gap seen between HINTN asserting and the read that answers it
    /// starting (HR-5). The hub deasserts and retries if left unserviced, so
    /// this is the margin against that, measured rather than assumed.
    std::uint32_t service_latency_max_us = 0;
};

/// The wiring, latch and clock both transports share.
class TransportCore {
   public:
    /// \param config the board wiring.
    /// \param bus the bus to strap for.
    /// \param sa0_high the I2C address strap level; ignored in SPI mode.
    TransportCore(const ControlConfig& config, Bus bus, bool sa0_high)
        : config_(config),
          control_({.nrst_pin = config.nrst_pin,
                    .ps0_pin = config.ps0_pin,
                    .ps1_pin = config.ps1_pin,
                    .bootn_pin = config.bootn_pin,
                    .sa0_pin = config.sa0_pin},
                   bus, sa0_high) {}

    /// \name Not copyable or movable
    /// Holds the HINTN latch, which the interrupt dispatch table points at.
    /// \{
    TransportCore(const TransportCore&) = delete;
    TransportCore& operator=(const TransportCore&) = delete;
    TransportCore(TransportCore&&) = delete;
    TransportCore& operator=(TransportCore&&) = delete;
    /// \}

    ~TransportCore() = default;

    /// Drive the straps with the hub held in reset (HR-3). The bus peripheral
    /// is the transport's own business and is configured between this and
    /// arm_interrupt() by its init().
    void begin_straps() { control_.init(); }

    /// Arm the HINTN interrupt, after the transport has configured its bus.
    ///
    /// \return TABLE_FULL when the interrupt dispatch table is full.
    pico_sh2::Result<void> arm_interrupt() {
        return hintn_.begin(config_.hintn_pin);
    }

    /// Release the HINTN interrupt.
    void end() { hintn_.end(); }

    /// Whether the hub has a transfer waiting: HINTN asserted.
    ///
    /// The live level, not the latched edge. The hub holds HINT asserted while
    /// it has a cargo to be read and deasserts it after the read begins
    /// (1000-3535 §2.6), so the level says a cargo is waiting and the edge
    /// says when (AR-2). An edge can outlive its assertion, and a read on the
    /// edge alone is a blind poll (HR-1). A missed or coalesced edge cannot
    /// wedge service(), since the read does not wait on one. The session
    /// bounds the number of transfers drained per pass, so a stuck-low line
    /// cannot spin the loop.
    [[nodiscard]] bool data_ready() const { return hintn_.asserted(); }

    /// Clear the pending edge ahead of a read.
    ///
    /// A read backed by a real edge also times the service gap: how long the
    /// hub waited between asserting HINTN and this read starting. A level
    /// fallback has no edge of its own to measure from, so timing it would
    /// report a gap that never happened.
    ///
    /// The gap runs from the edge that opened the span, not the newest one. An
    /// assertion left unserviced is retried by the hub, and measuring from the
    /// retry would report microseconds for the pass that missed its budget --
    /// the one case the figure exists to catch (HR-5). Read before the
    /// consume() that ends the span, so a concurrent edge cannot substitute the
    /// next span's start for this one's.
    ///
    /// \return whether a latched edge backed the read, and the edge the read
    ///         is anchored to (HR-1, AR-2).
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

    /// Drop an assertion a transfer this host has already performed answered:
    /// the second transfer of a cargo retrieved with a split read, or the SPI
    /// wake handshake's.
    ///
    /// The hub asserts once per transfer, and reasserts for each transfer of a
    /// cargo broken into several (1000-3535 §2.6). A transport that retrieves
    /// a cargo in one transfer never calls this; one that peeks the header and
    /// then reads the cargo has earned a second assertion and must account for
    /// it. The SPI wake handshake earns one the same way: the hub asserts to
    /// answer WAKE, and the write answers it. Left latched, the edge outlives
    /// its assertion: the next cargo's read counts as edged on it, and its
    /// service latency is measured from an assertion no cargo was waiting
    /// behind (HR-5, HR-6).
    ///
    /// Called after the answering transfer completes, by which time the
    /// assertion has been latched or, its line having risen, dropped. Only the
    /// flag is cleared, so an edge latched for a cargo offered meanwhile costs
    /// that read its edged status, never its anchor or the cargo (HR-1).
    void discard_continuation_assertion() {
        hintn_.discard_continuation_assertion();
    }

    /// Record a read the hub did not answer.
    void note_absent_read() { ++absent_reads_; }

    /// Start a fresh service-latency measurement window.
    ///
    /// The figure is a high-water mark, and startup puts an outlier in it that
    /// no later reading can displace: the session spends the reset hold and
    /// boot delay in timed waits by design, so an edge arriving mid-wait is
    /// serviced late for a reason that says nothing about whether the
    /// steady-state loop keeps up (HR-5). A caller measuring a run clears it
    /// first, so what it reads describes the run.
    void clear_service_latency_peak() { service_latency_max_us_ = 0; }

    /// Transfer, fault and latency counters for this transport.
    [[nodiscard]] TransportCounters counters() const {
        return {.level_fallback_reads = hintn_.level_fallback_reads(),
                .absent_reads = absent_reads_,
                .service_latency_max_us = service_latency_max_us_};
    }

    /// Drive NRST, active low.
    ///
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
