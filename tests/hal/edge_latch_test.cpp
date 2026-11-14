// SPDX-License-Identifier: MIT
// Publish edge flags and timestamps without torn values (AR-2).
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
    // Event timestamps still use the edge after its flag is consumed.
    EXPECT_EQ(latch.timestamp_us(), 500U);
}

// A read uses the edge returned by consume() (AR-2). A host test cannot
// insert the handler between consume()'s flag exchange and timestamp load.
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

// A fallback read can use an earlier edge and is counted (HR-1).
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

// Latency starts at the oldest unserviced edge, including across retries
// (HR-5).
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

// The completed second split-read transfer leaves no edge for a new read
// (HR-6).
TEST(EdgeLatch, DiscardingAContinuationAssertionLeavesNothingToService) {
    EdgeLatch latch;
    latch.latch(100);  // the cargo's own assertion
    EXPECT_TRUE(latch.consume().had_edge);
    latch.latch(300);  // reasserted for the second transfer

    latch.discard_continuation_assertion();
    EXPECT_FALSE(latch.ready());
}

TEST(EdgeLatch, DiscardingAContinuationAssertionCountsNoLevelFallback) {
    EdgeLatch latch;
    latch.latch(100);
    latch.discard_continuation_assertion();
    latch.discard_continuation_assertion();  // nothing latched: still not one
    EXPECT_EQ(latch.level_fallback_reads(), 0U);
}

// The next cargo opens a new latency period after an assertion is discarded
// (HR-5).
TEST(EdgeLatch, TheNextEdgeAfterADiscardOpensItsOwnSpan) {
    EdgeLatch latch;
    latch.latch(100);
    EXPECT_TRUE(latch.consume().had_edge);
    latch.latch(300);
    latch.discard_continuation_assertion();

    latch.latch(5'000);
    EXPECT_EQ(latch.pending_since_us(), 5'000U);
}

// The handler drops edges for assertions that have ended (HR-1).
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

// A dropped edge leaves the next live edge free to open its own period (HR-1).
TEST(EdgeLatch, ADroppedEdgeLeavesTheNextOneItsOwnSpan) {
    EdgeLatch latch;
    (void)latch.latch_if_asserted(1'000, false);
    (void)latch.latch_if_asserted(3'500, true);
    EXPECT_EQ(latch.pending_since_us(), 3'500U);
    EXPECT_TRUE(latch.consume().had_edge);
}

// Exercise the host API. Tearing safety on a 32-bit target still needs
// review of the seqlock strategy and interrupt ordering (AR-2).
TEST(EdgeLatch, ConcurrentReadsNeverSeeATornTimestamp) {
    EdgeLatch latch;
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> torn{0};

    // Matching halves make a torn value detectable.
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
