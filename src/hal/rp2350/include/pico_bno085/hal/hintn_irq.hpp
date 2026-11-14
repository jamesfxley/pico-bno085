// SPDX-License-Identifier: MIT
#pragma once

/// \file hintn_irq.hpp
/// SDK GPIO registration around EdgeLatch (AR-2).

#include <cstdint>

#include "pico_bno085/hal/edge_latch.hpp"
#include "pico_sh2/util/result.hpp"

namespace pico_bno085::hal {

/// Latch HINTN edges for service() and expose its live level (HR-1).
class HintnLatch {
   public:
    HintnLatch() = default;
    ~HintnLatch();

    /// \name Copy and move
    /// The interrupt table retains this object's address.
    /// \{
    HintnLatch(const HintnLatch&) = delete;
    HintnLatch& operator=(const HintnLatch&) = delete;
    HintnLatch(HintnLatch&&) = delete;
    HintnLatch& operator=(HintnLatch&&) = delete;
    /// \}

    /// Configure HINTN with a pull-up and falling-edge interrupt.
    /// Repeated calls reuse the registration. Dispatch changes preserve the
    /// bank state.
    /// \return TABLE_FULL if no dispatch slot is available. Enables the GPIO
    /// bank.
    pico_sh2::Result<void> begin(std::uint32_t pin);

    /// Release the registration, preserving the GPIO bank's enabled state.
    /// Safe before begin().
    void end();

    /// Whether active-low HINTN is asserted (1000-3535 §2.6).
    [[nodiscard]] bool asserted() const;

    /// Whether an edge has been latched and not yet consumed.
    [[nodiscard]] bool ready() const { return edge_.ready(); }

    /// Microsecond timestamp of the most recent latched edge.
    [[nodiscard]] std::uint64_t timestamp_us() const {
        return edge_.timestamp_us();
    }

    /// Start of the unserviced span in microseconds, or 0 before the first edge
    /// (HR-5).
    [[nodiscard]] std::uint64_t pending_since_us() const {
        return edge_.pending_since_us();
    }

    /// Clear the flag and return whether the read consumed an edge, and its
    /// timestamp.
    ConsumedEdge consume() { return edge_.consume(); }

    /// Reads made without a latched edge.
    [[nodiscard]] std::uint32_t level_fallback_reads() const {
        return edge_.level_fallback_reads();
    }

    /// Clear an assertion answered by a completed transfer (1000-3535 §2.6).
    void discard_continuation_assertion() {
        edge_.discard_continuation_assertion();
    }

    /// Latch from GPIO interrupt context only, unless HINTN has risen (HR-1).
    /// \param edge_us timestamp in microseconds captured at the edge.
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
