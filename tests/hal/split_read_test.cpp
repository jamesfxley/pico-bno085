// SPDX-License-Identifier: MIT
// HR-6: undoing the traces of a split read, and HR-2: telling a hub that left
// the bus apart from a bus that is wedged.
#include "pico_bno085/hal/split_read.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "pico_sh2/shtp/channels.hpp"

namespace {

using pico_bno085::hal::classify;
using pico_bno085::hal::SplitReadCompensator;
using pico_bno085::hal::TransferOutcome;

// A transfer as the hub re-offers it after a header peek: the continuation flag
// set, and the sequence number the hub has reached.
std::vector<std::uint8_t> reoffered(std::uint16_t length, std::uint8_t channel,
                                    std::uint8_t sequence) {
    const auto field =
        static_cast<std::uint16_t>(length | pico_sh2::shtp::CONTINUATION_MASK);
    return {static_cast<std::uint8_t>(static_cast<unsigned>(field) & 0xFFU),
            static_cast<std::uint8_t>(static_cast<unsigned>(field) >> 8U),
            channel, sequence};
}

TEST(Classify, MapsTheSdkCodesToBusMeaning) {
    // HR-2: an unacknowledged address is the hub leaving, not a fault.
    EXPECT_EQ(classify(pico_bno085::hal::SDK_ERROR_GENERIC),
              TransferOutcome::HUB_ABSENT);
    EXPECT_EQ(classify(pico_bno085::hal::SDK_ERROR_TIMEOUT),
              TransferOutcome::TIMEOUT);
    EXPECT_EQ(classify(-99), TransferOutcome::BUS_ERROR);
    EXPECT_EQ(classify(0), TransferOutcome::OK);
    EXPECT_EQ(classify(4), TransferOutcome::OK);
}

TEST(SplitRead, ClearsTheContinuationTheHostsOwnPeekCaused) {
    SplitReadCompensator compensator;
    auto transfer = reoffered(20, 3, 1);
    compensator.on_cargo(transfer, 3, false);

    // Bit 15 of the length field is gone: the engine sees a lead transfer.
    EXPECT_EQ(static_cast<unsigned>(transfer[1]) & 0x80U, 0);
    EXPECT_EQ(transfer[0], 20);
}

TEST(SplitRead, KeepsTheContinuationWhenTheHubStillHoldsARemainder) {
    SplitReadCompensator compensator;
    auto first = reoffered(600, 3, 1);
    compensator.on_cargo(first, 3, /*truncated=*/true);
    EXPECT_TRUE(compensator.cargo_incomplete());

    // The next transfer's flag is the hub's own reassembly state, not an
    // artefact of the peek, so it must survive.
    auto second = reoffered(300, 3, 3);
    compensator.on_cargo(second, 3, /*truncated=*/false);
    EXPECT_NE(static_cast<unsigned>(second[1]) & 0x80U, 0);
}

TEST(SplitRead, RestoresAOnePerCargoSequence) {
    SplitReadCompensator compensator;
    // The hub advances two sequence numbers per cargo (peek + body), so three
    // cargoes arrive numbered 1, 3, 5. The engine must see 0, 1, 2.
    std::array<std::uint8_t, 3> raw = {1, 3, 5};
    std::array<std::uint8_t, 3> seen{};
    for (std::size_t i = 0; i < raw.size(); ++i) {
        auto transfer = reoffered(20, 3, raw[i]);
        compensator.on_cargo(transfer, 3, false);
        seen[i] = transfer[3];
    }
    EXPECT_EQ(seen[0], 0);
    EXPECT_EQ(seen[1], 1);
    EXPECT_EQ(seen[2], 2);
}

TEST(SplitRead, LeavesAGenuineHubDropVisible) {
    SplitReadCompensator compensator;
    auto first = reoffered(20, 3, 1);
    compensator.on_cargo(first, 3, false);
    EXPECT_EQ(first[3], 0);

    // The hub dropped a cargo: it advanced by four rather than two.
    auto second = reoffered(20, 3, 5);
    compensator.on_cargo(second, 3, false);
    EXPECT_EQ(second[3], 3);  // a delta of 3, not 1 — the gap survives
}

TEST(SplitRead, TracksEachChannelSeparately) {
    SplitReadCompensator compensator;
    auto chan3 = reoffered(20, 3, 1);
    compensator.on_cargo(chan3, 3, false);
    auto chan4 = reoffered(20, 4, 1);
    compensator.on_cargo(chan4, 4, false);
    // Channel 4's own peek count is 1, not 2: the channels do not share it.
    EXPECT_EQ(chan3[3], 0);
    EXPECT_EQ(chan4[3], 0);
}

// A null header is not a cargo, so the hub's sequence number does not move for
// it (1000-3535 §2.2, §2.3.1). The BNO086 answers every idle poll this way, so
// treating one as a consumed sequence would desynchronise the correction on a
// bus that is behaving perfectly.
TEST(SplitRead, ANullHeaderConsumesNoSequenceNumber) {
    SplitReadCompensator compensator;
    auto first = reoffered(20, 3, 1);
    compensator.on_cargo(first, 3, false);
    EXPECT_EQ(first[3], 0);

    for (int i = 0; i < 10; ++i) {
        compensator.on_null_header();
    }

    // The next real cargo is still the hub's next sequence number.
    auto second = reoffered(20, 3, 3);
    compensator.on_cargo(second, 3, false);
    EXPECT_EQ(second[3], 1);
}

TEST(SplitRead, HubAbsenceDropsTheStateTheHubAbandoned) {
    SplitReadCompensator compensator;
    auto first = reoffered(600, 3, 1);
    compensator.on_cargo(first, 3, /*truncated=*/true);
    ASSERT_TRUE(compensator.cargo_incomplete());

    compensator.on_hub_absent();
    EXPECT_FALSE(compensator.cargo_incomplete());

    // The hub restarts its sequence numbers, and so does the correction.
    auto after = reoffered(20, 3, 1);
    compensator.on_cargo(after, 3, false);
    EXPECT_EQ(static_cast<unsigned>(after[1]) & 0x80U,
              0);  // continuation cleared again
    EXPECT_EQ(after[3], 0);
}

TEST(SplitRead, IgnoresATransferShorterThanAHeader) {
    SplitReadCompensator compensator;
    std::vector<std::uint8_t> tiny = {0x01, 0x02};
    compensator.on_cargo(tiny, 3,
                         false);  // must not read or write past the end
    EXPECT_EQ(tiny.size(), 2U);
}

TEST(SplitRead, IgnoresAChannelOutsideTheMap) {
    SplitReadCompensator compensator;
    auto transfer = reoffered(20, 200, 7);
    compensator.on_cargo(transfer, 200, false);
    EXPECT_EQ(transfer[3], 7);  // sequence untouched
    EXPECT_EQ(static_cast<unsigned>(transfer[1]) & 0x80U, 0);
}

}  // namespace
