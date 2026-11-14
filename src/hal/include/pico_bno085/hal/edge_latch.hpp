// SPDX-License-Identifier: MIT
#pragma once

/// \file edge_latch.hpp
/// Interrupt-to-service publication of HINTN edges (AR-2).
/// The timestamp must be captured at the edge, not at the deferred read.

#include <atomic>
#include <cstdint>

#include "pico_util/publish/published.hpp"

namespace pico_bno085::hal {

/// Whether a read consumed an edge, and its timestamp.
struct ConsumedEdge {
    /// False when the read was served by the level fallback alone (HR-1).
    bool had_edge = false;
    /// Newest latched edge in microseconds, or 0 before the first edge.
    /// A fallback can carry an earlier edge unless a fresh edge arrives during
    /// consume().
    std::uint64_t edge_us = 0;
};

/// Publish HINTN edges from interrupt context to service context.
class EdgeLatch {
   public:
    /// Whether an edge has been latched and not yet consumed.
    [[nodiscard]] bool ready() const {
        return flag_.load(std::memory_order_acquire);
    }

    /// The newest latched edge in microseconds, or 0 before the first edge.
    [[nodiscard]] std::uint64_t timestamp_us() const {
        return timestamp_.load();
    }

    /// Start of the unserviced span, its oldest unanswered edge, in
    /// microseconds, or 0 before the first edge (HR-5). Read before consume(),
    /// which ends it.
    [[nodiscard]] std::uint64_t pending_since_us() const {
        return pending_since_.load();
    }

    /// Clear the flag and take the timestamp for this read (AR-2).
    /// \return The edge and whether it was latched. Fallbacks are counted
    /// (HR-1).
    ConsumedEdge consume() {
        const bool had_edge = flag_.exchange(false, std::memory_order_acq_rel);
        if (!had_edge) {
            ++level_fallback_reads_;
        }
        // Load the timestamp after clearing the flag. A concurrent edge can
        // then make a fallback carry a fresh edge, but cannot make an edged
        // read carry the previous edge.
        return {.had_edge = had_edge, .edge_us = timestamp_.load()};
    }

    /// Reads made without a latched edge.
    [[nodiscard]] std::uint32_t level_fallback_reads() const {
        return level_fallback_reads_;
    }

    /// Clear an assertion answered by a completed transfer (1000-3535 §2.6).
    /// Timestamps and fallback count stay unchanged (HR-5, HR-1, HR-6).
    void discard_continuation_assertion() {
        flag_.store(false, std::memory_order_release);
    }

    /// Store an edge from interrupt context only.
    /// \param edge_us timestamp in microseconds captured at the edge.
    void latch(std::uint64_t edge_us) {
        timestamp_.store(edge_us);
        // Relaxed: service context cannot preempt the handler. Any later edge
        // leaves the span start intact. A concurrent cross-core consume()
        // can over-report latency. Cross-core support is unconfirmed (FW-3).
        if (!flag_.load(std::memory_order_relaxed)) {
            pending_since_.store(edge_us);
        }
        // Publish the flag after both timestamps so an acquiring reader sees
        // them.
        flag_.store(true, std::memory_order_release);
    }

    /// Latch an edge only while HINTN remains asserted (HR-1, HR-5).
    /// Called only from the handler. Ended assertions include retries
    /// (1000-3535 §2.6).
    /// \param edge_us timestamp in microseconds captured at the edge.
    /// \param line_asserted whether HINTN remains asserted.
    /// \return whether the edge was latched.
    bool latch_if_asserted(std::uint64_t edge_us, bool line_asserted) {
        if (!line_asserted) {
            return false;
        }
        latch(edge_us);
        return true;
    }

    /// Drop any pending edge. For a transport re-initialising its latch.
    void reset() { flag_.store(false, std::memory_order_release); }

   private:
    /// The handler writes and mainline reads. The seqlock reader cannot preempt
    /// its writer, so waiting for publication terminates.
    /// The opposite preemption direction needs the opposite strategy.
    /// A host test cannot tell them apart.
    pico_util::Published<
        std::uint64_t,
        pico_util::PublishStrategy::SEQLOCK_WRITER_NOT_PREEMPTIBLE>
        timestamp_;

    /// The unserviced span's first edge, published by the handler to mainline.
    pico_util::Published<
        std::uint64_t,
        pico_util::PublishStrategy::SEQLOCK_WRITER_NOT_PREEMPTIBLE>
        pending_since_;

    /// The consumable flag is separate from timestamp publication.
    std::atomic<bool> flag_{false};

    // Written only from service() context (consume()), never from the handler.
    std::uint32_t level_fallback_reads_ = 0;
};

}  // namespace pico_bno085::hal
