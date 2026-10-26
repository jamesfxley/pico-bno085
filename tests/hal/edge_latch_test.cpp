// SPDX-License-Identifier: MIT
// AR-2: the edge flag and its timestamp, published so a reader can never
// observe a half-written value.
#include "pico_bno085/hal/edge_latch.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>

namespace {

using pico_bno085::hal::EdgeLatch;

TEST(EdgeLatch, StartsWithNothingLatched) {
    const EdgeLatch latch;
    EXPECT_FALSE(latch.ready());
    EXPECT_EQ(latch.timestamp_us(), 0U);
    EXPECT_EQ(latch.level_fallback_reads(), 0U);
}

TEST(EdgeLatch, LatchPublishesTheEdgeAndItsTime) {
    EdgeLatch latch;
    latch.latch(123'456);
    EXPECT_TRUE(latch.ready());
    EXPECT_EQ(latch.timestamp_us(), 123'456U);
}

TEST(EdgeLatch, ConsumeClearsTheFlagAndReturnsTheEdge) {
    EdgeLatch latch;
    latch.latch(500);
    const auto consumed = latch.consume();
    EXPECT_TRUE(consumed.had_edge);
    EXPECT_EQ(consumed.edge_us, 500U);
    EXPECT_FALSE(latch.ready());
    // The events decoded from this transfer are anchored to the edge, so the
    // timestamp has to outlive the flag.
    EXPECT_EQ(latch.timestamp_us(), 500U);
}

// AR-2: a read is anchored to the edge consume() returns, not to one asked for
// before the read. A later edge latched before the read clears the flag is that
// read's anchor, and the read counts as edged. The order inside consume() --
// the flag exchanged before the timestamp is loaded -- is not reachable from a
// host test, which cannot put the handler between the two.
TEST(EdgeLatch, ConsumeAnchorsTheReadToTheEdgeLatchedBeforeIt) {
    EdgeLatch latch;
    latch.latch(10'000);
    (void)latch.consume();

    latch.latch(12'500);

    const auto consumed = latch.consume();
    EXPECT_TRUE(consumed.had_edge);
    EXPECT_EQ(consumed.edge_us, 12'500U);
    EXPECT_EQ(latch.level_fallback_reads(), 0U);
}

// HR-1: a read with no latched edge behind it is served by the level fallback
// and anchors to an earlier edge, so it is counted rather than hidden.
TEST(EdgeLatch, ConsumeWithoutAnEdgeCountsALevelFallback) {
    EdgeLatch latch;
    EXPECT_FALSE(latch.consume().had_edge);
    EXPECT_EQ(latch.level_fallback_reads(), 1U);

    latch.latch(10);
    EXPECT_TRUE(latch.consume().had_edge);
    EXPECT_EQ(latch.level_fallback_reads(), 1U);  // an edge is not a fallback

    const auto fallback = latch.consume();
    EXPECT_FALSE(fallback.had_edge);
    EXPECT_EQ(fallback.edge_us, 10U);  // anchored to the earlier edge
    EXPECT_EQ(latch.level_fallback_reads(), 2U);
}

TEST(EdgeLatch, ResetDropsAPendingEdge) {
    EdgeLatch latch;
    latch.latch(42);
    ASSERT_TRUE(latch.ready());
    latch.reset();
    EXPECT_FALSE(latch.ready());
}

TEST(EdgeLatch, ASecondEdgeReplacesAnUnconsumedOne) {
    EdgeLatch latch;
    latch.latch(100);
    latch.latch(200);
    EXPECT_TRUE(latch.ready());
    EXPECT_EQ(latch.timestamp_us(), 200U);
}

// HR-5: the span an unserviced assertion opens is what a service-latency
// figure measures from. The hub retries an assertion left unserviced, so
// measuring from the newest edge would report the gap since the retry -- a few
// microseconds -- for exactly the pass that overran.
TEST(EdgeLatch, ASecondEdgeDoesNotMoveTheSpanStart) {
    EdgeLatch latch;
    latch.latch(100);
    latch.latch(200);
    EXPECT_EQ(latch.pending_since_us(), 100U);
    EXPECT_EQ(latch.timestamp_us(), 200U);
}

TEST(EdgeLatch, ConsumeEndsTheSpanSoTheNextEdgeOpensANewOne) {
    EdgeLatch latch;
    latch.latch(100);
    latch.latch(200);
    EXPECT_TRUE(latch.consume().had_edge);

    latch.latch(300);
    EXPECT_EQ(latch.pending_since_us(), 300U);
}

TEST(EdgeLatch, StartsWithNoSpanOpen) {
    const EdgeLatch latch;
    EXPECT_EQ(latch.pending_since_us(), 0U);
}

// HR-6: the hub asserts once per transfer, so an I2C cargo retrieved by a
// header peek and a cargo read earns two assertions. The second announces the
// transfer just completed; left latched, it would back the next read as edged
// and open the span its service latency is measured from.
TEST(EdgeLatch, DiscardingAContinuationAssertionLeavesNothingToService) {
    EdgeLatch latch;
    latch.latch(100);  // the cargo's own assertion
    EXPECT_TRUE(latch.consume().had_edge);
    latch.latch(300);  // reasserted for the second transfer

    latch.discard_continuation_assertion();
    EXPECT_FALSE(latch.ready());
}

// It answers no read, so it is not a read served by the level fallback.
TEST(EdgeLatch, DiscardingAContinuationAssertionCountsNoLevelFallback) {
    EdgeLatch latch;
    latch.latch(100);
    latch.discard_continuation_assertion();
    latch.discard_continuation_assertion();  // nothing latched: still not one
    EXPECT_EQ(latch.level_fallback_reads(), 0U);
}

// HR-5: the discarded assertion opened no span, so the next cargo's assertion
// starts the measurement rather than continuing one that no read answered.
TEST(EdgeLatch, TheNextEdgeAfterADiscardOpensItsOwnSpan) {
    EdgeLatch latch;
    latch.latch(100);
    EXPECT_TRUE(latch.consume().had_edge);
    latch.latch(300);
    latch.discard_continuation_assertion();

    latch.latch(5'000);
    EXPECT_EQ(latch.pending_since_us(), 5'000U);
}

// HR-1: an edge whose line has already risen announced an assertion that has
// ended, so the handler drops it. Latched, it would back the next read as edged
// and open the span that read's service latency is measured from.
TEST(EdgeLatch, AnEdgeWhoseLineHasRisenIsNotLatched) {
    EdgeLatch latch;
    EXPECT_FALSE(latch.latch_if_asserted(1'000, false));
    EXPECT_FALSE(latch.ready());
    EXPECT_EQ(latch.timestamp_us(), 0U);
    EXPECT_EQ(latch.pending_since_us(), 0U);
}

TEST(EdgeLatch, AnEdgeWhoseLineIsStillAssertedIsLatched) {
    EdgeLatch latch;
    EXPECT_TRUE(latch.latch_if_asserted(1'000, true));
    EXPECT_TRUE(latch.ready());
    EXPECT_EQ(latch.timestamp_us(), 1'000U);
    EXPECT_EQ(latch.pending_since_us(), 1'000U);
}

// HR-1: a dropped edge leaves the span alone, so the next live edge opens its
// own rather than inheriting one from an assertion already answered.
TEST(EdgeLatch, ADroppedEdgeLeavesTheNextOneItsOwnSpan) {
    EdgeLatch latch;
    (void)latch.latch_if_asserted(1'000, false);
    (void)latch.latch_if_asserted(3'500, true);
    EXPECT_EQ(latch.pending_since_us(), 3'500U);
    EXPECT_TRUE(latch.consume().had_edge);
}

// AR-2: host API exercise, not proof of 64-bit tearing safety on a 32-bit
// target. Review checks the seqlock strategy and interrupt/mainline order.
TEST(EdgeLatch, ConcurrentReadsNeverSeeATornTimestamp) {
    EdgeLatch latch;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> torn{0};

    // Each value's high and low halves carry the same counter, so any mixture
    // of two different writes is detectable.
    auto compose = [](std::uint32_t n) {
        return (static_cast<std::uint64_t>(n) << 32U) | n;
    };

    std::thread writer([&] {
        for (std::uint32_t value = 1; !stop.load(std::memory_order_relaxed);
             ++value) {
            latch.latch(compose(value));
        }
    });

    for (int i = 0; i < 200'000; ++i) {
        const std::uint64_t packed = latch.timestamp_us();
        const auto high = static_cast<std::uint32_t>(packed >> 32U);
        const auto low = static_cast<std::uint32_t>(packed & 0xFFFFFFFFU);
        if (high != low) {
            torn.store(packed, std::memory_order_relaxed);
            break;
        }
    }

    stop.store(true, std::memory_order_relaxed);
    writer.join();
    EXPECT_EQ(torn.load(), 0U) << "observed a torn timestamp";
}

}  // namespace
