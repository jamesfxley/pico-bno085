// SPDX-License-Identifier: MIT
#pragma once

/// \file hintn_irq.hpp
/// HINTN falling-edge latch: the GPIO wiring around EdgeLatch, which holds the
/// edge itself and the protocol for publishing it (AR-2).
///
/// Firmware-only: it uses SDK GPIO interrupts.

#include <cstdint>

#include "pico_bno085/hal/edge_latch.hpp"
#include "pico_sh2/util/result.hpp"

namespace pico_bno085::hal {

/// A HINTN falling-edge interrupt, latched for a later service() pass.
///
/// Wraps EdgeLatch, which owns the edge and its publication protocol, and adds
/// the SDK GPIO registration plus the live level, which is what says a cargo is
/// waiting (HR-1).
///
/// Registration claims only the HINTN pin, through the SDK's raw GPIO handler
/// rather than the core's single GPIO callback, so an application's own
/// interrupts on other pins keep working and this one survives them being
/// armed in either order.
class HintnLatch {
   public:
    HintnLatch() = default;
    ~HintnLatch();

    /// \name Not copyable or movable
    /// The interrupt dispatch table holds a pointer to this object, so it
    /// cannot move once registered.
    /// \{
    HintnLatch(const HintnLatch&) = delete;
    HintnLatch& operator=(const HintnLatch&) = delete;
    HintnLatch(HintnLatch&&) = delete;
    HintnLatch& operator=(HintnLatch&&) = delete;
    /// \}

    /// Configure `pin` as an input with pull-up and enable a falling-edge
    /// interrupt routed to this latch.
    ///
    /// Registering is idempotent: calling begin() again — re-initialising a
    /// transport, or reconfiguring after a bus change — re-registers in place
    /// rather than consuming another dispatch slot.
    ///
    /// \param pin the GPIO carrying HINTN.
    /// \return TABLE_FULL when the dispatch table is full, which is a wiring
    ///         mistake rather than a runtime condition, so it is surfaced
    ///         instead of dropped.
    /// Enables the GPIO interrupt bank; dispatch changes preserve its state.
    pico_sh2::Result<void> begin(std::uint32_t pin);

    /// Disable the interrupt and drop the registration. Safe to call when not
    /// begun.
    /// Releasing this latch preserves the GPIO bank's enabled state.
    void end();

    /// Whether HINTN is currently asserted, read from the live pin level
    /// (active low).
    ///
    /// What data_ready() reads: the hub holds HINTN asserted while a cargo
    /// waits (1000-3535 §2.6), so a missed or coalesced edge cannot wedge
    /// service(). Defined in the .cpp, which needs the SDK GPIO read.
    [[nodiscard]] bool asserted() const;

    /// Whether an edge has been latched and not yet consumed.
    [[nodiscard]] bool ready() const { return edge_.ready(); }

    /// Microsecond timestamp of the most recent latched edge.
    [[nodiscard]] std::uint64_t timestamp_us() const {
        return edge_.timestamp_us();
    }

    /// Microsecond timestamp of the edge that opened the current unserviced
    /// span, which is what the service-latency figure measures from (HR-5).
    [[nodiscard]] std::uint64_t pending_since_us() const {
        return edge_.pending_since_us();
    }

    /// Clear the flag ahead of a read.
    ///
    /// \return whether a latched edge backed this read, and the edge it is
    ///         anchored to (EdgeLatch::consume).
    ConsumedEdge consume() { return edge_.consume(); }

    /// Count of reads served by the level fallback with no latched edge behind
    /// them.
    [[nodiscard]] std::uint32_t level_fallback_reads() const {
        return edge_.level_fallback_reads();
    }

    /// Drop an assertion a transfer already performed has answered
    /// (1000-3535 §2.6).
    void discard_continuation_assertion() {
        edge_.discard_continuation_assertion();
    }

    /// Store the edge, unless the line has already risen (HR-1). Called only
    /// from the GPIO interrupt.
    ///
    /// \param edge_us microsecond timestamp captured at the edge.
    void latch(std::uint64_t edge_us) {
        (void)edge_.latch_if_asserted(edge_us, asserted());
    }

    /// The GPIO this latch is registered on.
    [[nodiscard]] std::uint32_t pin() const { return pin_; }

    /// Whether begin() has registered this latch.
    [[nodiscard]] bool begun() const { return begun_; }

   private:
    EdgeLatch edge_;
    std::uint32_t pin_ = 0;
    bool begun_ = false;
};

}  // namespace pico_bno085::hal
