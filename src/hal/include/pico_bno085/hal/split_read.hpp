// SPDX-License-Identifier: MIT
#pragma once

/// \file split_read.hpp
/// The protocol bookkeeping an I2C retrieval needs, separated from the SDK
/// calls that drive the bus.
///
/// Retrieving a cargo over I2C takes two reads — a four-byte peek to learn the
/// length, then a read of the whole cargo — and the hub answers that split with
/// a continuation flag and an extra sequence number that describe the host's
/// read pattern rather than the cargo. Undoing them is protocol reasoning, and
/// keeping it here means it can be tested on the host against the specification
/// instead of only against a wired board.
///
/// Header-free of the Pico SDK by design; I2cHalTransport supplies the bus.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "pico_sh2/shtp/channels.hpp"

namespace pico_bno085::hal {

/// What an SDK transfer return code says about the bus.
enum class TransferOutcome : std::uint8_t {
    OK,          ///< The transfer moved bytes.
    HUB_ABSENT,  ///< The address was not acknowledged: no device answering.
    TIMEOUT,     ///< The transaction ran out of time: stretched or wedged.
    BUS_ERROR,   ///< Anything else.
};

/// `PICO_ERROR_GENERIC`, restated so this header needs no SDK include and the
/// classification stays host-testable. `i2c_hal_transport.cpp` static_asserts
/// it against `hardware/i2c.h`, so a divergence is a build failure rather than
/// a surprise on the bench.
inline constexpr int SDK_ERROR_GENERIC = -1;
/// `PICO_ERROR_TIMEOUT`, restated for the same reason as SDK_ERROR_GENERIC.
inline constexpr int SDK_ERROR_TIMEOUT = -2;

/// Classify an SDK transfer return code.
///
/// `PICO_ERROR_GENERIC` is "address not acknowledged, no device present" and
/// `PICO_ERROR_TIMEOUT` is a transaction that ran out of time
/// (`hardware/i2c.h`). The first says the hub left the bus, the second says the
/// bus is stretched or wedged; only the second is a fault (HR-2).
///
/// \param rc the SDK return code: non-negative is a byte count.
/// \return what that code says about the bus.
constexpr TransferOutcome classify(int rc) {
    if (rc >= 0) {
        return TransferOutcome::OK;
    }
    switch (rc) {
        case SDK_ERROR_GENERIC:
            return TransferOutcome::HUB_ABSENT;
        case SDK_ERROR_TIMEOUT:
            return TransferOutcome::TIMEOUT;
        default:
            return TransferOutcome::BUS_ERROR;
    }
}

/// Offset of the length field's high byte within the 4-byte SHTP header
/// (1000-3535 §2.2).
inline constexpr std::size_t HEADER_LENGTH_MSB = 1;
/// Offset of the sequence-number byte within the 4-byte SHTP header.
inline constexpr std::size_t HEADER_SEQUENCE = 3;

/// Removes the traces of the host's own split read from a retrieved cargo, so
/// the framing engine — which never sees the header peek — reads the same bytes
/// it would have seen from a single-transfer retrieval (HR-6).
class SplitReadCompensator {
   public:
    /// A complete cargo, retrieved by a header peek followed by a body read.
    ///
    /// \param transfer the staged cargo, corrected in place.
    /// \param channel the SHTP channel the cargo arrived on.
    /// \param truncated the cargo did not fit the staging span, so the hub
    ///        still holds the remainder.
    ///
    /// Two corrections, both from 1000-3535 §2.3.1:
    ///
    /// The peek terminates after the length field, so the hub re-offers the
    /// cargo from its start with the continuation flag set. That flag describes
    /// this host's split read, not a fragmented cargo, and the engine would
    /// drop the transfer as an out-of-order fragment. It is cleared — unless
    /// the previous transfer left a cargo genuinely unfinished, in which case
    /// the flag is the hub's own and carries reassembly state the engine must
    /// act on.
    ///
    /// "The sequence number is incremented for each read", and retrieving one
    /// cargo takes two, so the engine — seeing only the second — would score
    /// every cargo as a dropped one. Subtracting the running count of numbers
    /// the peeks have consumed on this channel restores a one-per-cargo
    /// sequence, and leaves a genuine hub-side drop visible as a delta above
    /// one.
    void on_cargo(std::span<std::uint8_t> transfer, std::uint8_t channel,
                  bool truncated) {
        if (transfer.size() < pico_sh2::shtp::HEADER_SIZE) {
            return;
        }
        if (!cargo_incomplete_) {
            // Widened explicitly throughout: std::uint8_t and std::uint16_t
            // integer-promote to signed int, so the natural spelling applies
            // ~ and & to a signed value.
            constexpr auto CONTINUATION_IN_MSB = static_cast<std::uint8_t>(
                static_cast<unsigned>(pico_sh2::shtp::CONTINUATION_MASK) >> 8U);
            transfer[HEADER_LENGTH_MSB] = static_cast<std::uint8_t>(
                static_cast<unsigned>(transfer[HEADER_LENGTH_MSB]) &
                ~static_cast<unsigned>(CONTINUATION_IN_MSB));
        }
        cargo_incomplete_ = truncated;

        if (channel < pico_sh2::shtp::MAX_TRACKED_CHANNELS) {
            ++peek_sequences_[channel];
            transfer[HEADER_SEQUENCE] = static_cast<std::uint8_t>(
                transfer[HEADER_SEQUENCE] - peek_sequences_[channel]);
        }
    }

    /// A read the hub answered with a null header. Deliberately no correction:
    /// the sequence number "increments once for each cargo sent or cargo
    /// continuation sent" (1000-3535 §2.2) and a null header means "there is no
    /// cargo" (§2.3.1), so an empty read consumes no sequence number and leaves
    /// nothing to undo. This is the BNO086's answer to a poll with no data
    /// pending, so it happens once per idle service pass.
    void on_null_header() {}

    /// The hub stopped acknowledging: it has left the bus to reset. It abandons
    /// any part-sent cargo and restarts its sequence numbers while away, so the
    /// state carried across transfers no longer describes anything.
    void on_hub_absent() {
        cargo_incomplete_ = false;
        peek_sequences_.fill(0);
    }

    /// Whether the last cargo did not fit and the hub still holds a remainder,
    /// so the next transfer's continuation flag is the hub's own.
    [[nodiscard]] bool cargo_incomplete() const { return cargo_incomplete_; }

   private:
    bool cargo_incomplete_ = false;

    // Per-channel count of sequence numbers consumed by header peeks, wrapping
    // with the 8-bit sequence field it corrects.
    std::array<std::uint8_t, pico_sh2::shtp::MAX_TRACKED_CHANNELS>
        peek_sequences_{};
};

}  // namespace pico_bno085::hal
