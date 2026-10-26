// SPDX-License-Identifier: MIT
#pragma once

/// \file edge_latch.hpp
/// The interrupt-to-service handoff behind the HINTN latch: a flag and the
/// microsecond timestamp of the edge, published so a reader can never observe a
/// half-written value.
///
/// AR-2: the interrupt handler stores the flag and the timestamp, then returns
/// — no bus I/O, no library calls. The timestamp is the anchor the SH-2 base
/// timestamp (0xFB) delta is relative to, so it must be captured at the edge
/// and not when the deferred read runs.
///
/// Free of the Pico SDK so the publication protocol can be tested on the host,
/// including under real concurrency; HintnLatch adds the GPIO wiring.

#include <atomic>
#include <cstdint>

#include "pico_util/publish/published.hpp"

namespace pico_bno085::hal {

/// What a read found when it cleared the latch: whether an edge backed it, and
/// the edge it is anchored to.
struct ConsumedEdge {
    /// False when the read was served by the level fallback alone (HR-1).
    bool had_edge = false;
    /// Microsecond timestamp of the edge the read is anchored to: the newest
    /// latched when consume() read it. For a level fallback that is an earlier
    /// edge, unless the handler latched a fresh one between the flag's
    /// clearing and that read. 0 before the first edge.
    std::uint64_t edge_us = 0;
};

/// Publishes one HINTN edge from interrupt context to service context.
///
/// The flag and the edge timestamp are written by latch() in the handler and
/// read by ready(), timestamp_us() and consume() from service context. The
/// timestamp is 64 bits and the target stores it as two 32-bit words, so it is
/// published under a seqlock rather than read directly -- that protocol is
/// pico_util::Published's, and what belongs here is the latch policy around
/// it: the consumable flag and the level-fallback count.
class EdgeLatch {
   public:
    /// Whether an edge has been latched and not yet consumed.
    [[nodiscard]] bool ready() const {
        return flag_.load(std::memory_order_acquire);
    }

    /// Microsecond timestamp of the most recent latched edge.
    ///
    /// \return the edge timestamp in microseconds, or 0 before the first edge.
    [[nodiscard]] std::uint64_t timestamp_us() const {
        return timestamp_.load();
    }

    /// Microsecond timestamp of the edge that opened the current unserviced
    /// span -- the oldest edge not yet answered by a read.
    ///
    /// This is what a service-latency figure has to measure from (HR-5).
    /// timestamp_us() is the newest edge, and the hub deasserts and retries an
    /// assertion left unserviced, so a pass that missed its budget provokes a
    /// retry whose fresh edge would restart the measurement -- reporting
    /// microseconds for exactly the run that overran.
    ///
    /// Read it before consume(), which ends the span: an edge arriving between
    /// the two would open the next span, and the reading would be of that one.
    ///
    /// \return the span-start timestamp in microseconds, or 0 before the first
    ///         edge.
    [[nodiscard]] std::uint64_t pending_since_us() const {
        return pending_since_.load();
    }

    /// Clear the flag ahead of a read, and report whether a latched edge
    /// backed it and which edge the read is anchored to.
    ///
    /// The edge is read here, in the step that clears the flag, rather than
    /// asked for beforehand. A reader that sees the line asserted can get there
    /// before the handler has latched the edge behind it; an anchor taken then
    /// is the previous edge's, and a consume() after the handler has run would
    /// report the read as edged, so nothing would count it (AR-2).
    ///
    /// \return the edge the read is anchored to. had_edge false means the read
    /// is served by the level fallback alone, so edge_us is usually an earlier
    /// edge and the events decoded from this transfer are anchored to it; the
    /// case is counted, an upper bound on such reads, so a device run can show
    /// how often it occurs (HR-1).
    ConsumedEdge consume() {
        const bool had_edge = flag_.exchange(false, std::memory_order_acq_rel);
        if (!had_edge) {
            ++level_fallback_reads_;
        }
        // After the exchange, not before it. An edge the handler latches
        // between the two makes a read counted as a fallback that carries the
        // right edge; read first, the same interleaving makes a read counted
        // as edged that carries the wrong one.
        return {.had_edge = had_edge, .edge_us = timestamp_.load()};
    }

    /// Count of reads served by the level fallback with no latched edge behind
    /// them.
    [[nodiscard]] std::uint32_t level_fallback_reads() const {
        return level_fallback_reads_;
    }

    /// Drop an assertion a transfer the host has already performed answered.
    ///
    /// The hub asserts once per transfer, not once per cargo: "if a cargo is
    /// broken up into multiple reads then the hub deasserts and reasserts HINT
    /// for each transfer" (1000-3535 §2.6). A host that retrieves one cargo in
    /// two transfers is therefore owed two assertions, and the second
    /// announces the transfer it has just finished rather than a new cargo.
    /// The SPI wake handshake is the same case: the hub asserts to answer
    /// WAKE, and the write answers the assertion. Left latched, either
    /// outlives its assertion: the next cargo's read counts as edged on it,
    /// and its service latency is measured from it, which is a figure about
    /// the host's own transfers rather than about keeping up (HR-5).
    ///
    /// Not a consume(): it answers no read, so it counts no level fallback and
    /// leaves the edge timestamps alone. Only the flag is cleared, so a cargo
    /// the hub is genuinely holding still reaches service() through the level
    /// fallback even in the coalesced case where its assertion and the
    /// discarded one share the flag (HR-1, HR-6).
    void discard_continuation_assertion() {
        flag_.store(false, std::memory_order_release);
    }

    /// Store the edge. Called only from the interrupt handler.
    ///
    /// \param edge_us microsecond timestamp captured at the edge.
    void latch(std::uint64_t edge_us) {
        timestamp_.store(edge_us);
        // Only an edge that finds nothing pending opens a span; a second edge
        // arriving before the first is answered leaves the span start where it
        // is, which is the whole point of holding it separately.
        //
        // Relaxed: the handler is the only writer of both, and it is not
        // preemptible by the service context that reads them, so there is no
        // ordering for this load to establish. A reader on the other core can
        // see a span start whose span a concurrent consume() has just ended,
        // which over-reports the gap. Over-reporting is the safe direction for
        // a gate -- it cannot turn a missed budget into a passing one.
        if (!flag_.load(std::memory_order_relaxed)) {
            pending_since_.store(edge_us);
        }
        // After the timestamps, so a reader that sees the flag set sees the
        // edge that set it. The strategy's own release ordering publishes
        // them; this orders the flag behind them.
        flag_.store(true, std::memory_order_release);
    }

    /// Store the edge if its assertion is still in progress. Called only from
    /// the interrupt handler, with the line level it read after the edge.
    ///
    /// A line already risen means the assertion has ended: answered by a read
    /// the handler was held off past, or dropped by the hub's retry
    /// (1000-3535 §2.6). Latched, the edge would outlive it, backing the next
    /// read as edged and opening the span that read's service latency is
    /// measured from (HR-1, HR-5). A retry reasserts, and its edge is latched;
    /// the timestamp of a late handler is its own, not the edge's, anyway.
    ///
    /// \param edge_us microsecond timestamp captured at the edge.
    /// \param line_asserted whether HINTN was still asserted when read.
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
    /// The edge timestamp, published from the handler to service context.
    ///
    /// SEQLOCK_WRITER_NOT_PREEMPTIBLE, named rather than defaulted: the writer
    /// is the interrupt handler and the reader is mainline, so a reader that
    /// spins waiting for the writer always terminates -- the handler runs to
    /// completion. The opposite direction needs the opposite strategy, and
    /// neither is distinguishable in a host test.
    pico_util::Published<
        std::uint64_t,
        pico_util::PublishStrategy::SEQLOCK_WRITER_NOT_PREEMPTIBLE>
        timestamp_;

    /// The timestamp of the edge that opened the current unserviced span,
    /// published the same way and for the same reason as \ref timestamp_.
    pico_util::Published<
        std::uint64_t,
        pico_util::PublishStrategy::SEQLOCK_WRITER_NOT_PREEMPTIBLE>
        pending_since_;

    /// Whether an edge is latched and unconsumed. Atomic rather than volatile:
    /// it is written by the handler and read-modify-written by service, and
    /// volatile orders nothing between contexts (CXX-08.1). Not part of the
    /// published value, because it is consumed and the publication flag is
    /// not.
    std::atomic<bool> flag_{false};

    // Written only from service() context (consume()), never from the handler.
    std::uint32_t level_fallback_reads_ = 0;
};

}  // namespace pico_bno085::hal
