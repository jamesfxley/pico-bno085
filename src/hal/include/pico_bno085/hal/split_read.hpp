// SPDX-License-Identifier: MIT
#pragma once

/// \file split_read.hpp
/// Compensate for I2C header peeks without SDK dependencies.

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

/// SDK generic error code, checked against the SDK in i2c_hal_transport.cpp.
inline constexpr int SDK_ERROR_GENERIC = -1;
/// SDK timeout error code, checked in i2c_hal_transport.cpp.
inline constexpr int SDK_ERROR_TIMEOUT = -2;

/// Classify an SDK byte count or error.
/// \param rc non-negative byte count, or a negative SDK error.
/// \return HUB_ABSENT for an unacknowledged address, TIMEOUT for expiry (HR-2).
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

/// Correct a retrieved cargo's continuation flag and sequence number
/// for the host's header peek (HR-6).
class SplitReadCompensator {
   public:
    /// Correct a cargo in place after a header peek (1000-3535 §2.3.1).
    /// A genuinely incomplete cargo keeps the continuation flag the engine must
    /// act on.
    /// \param transfer the staged cargo, corrected in place.
    /// \param channel the SHTP channel.
    /// \param truncated whether the hub still holds bytes beyond the staging
    /// span.
    void on_cargo(std::span<std::uint8_t> transfer, std::uint8_t channel,
                  bool truncated) {
        if (transfer.size() < pico_sh2::shtp::HEADER_SIZE) {
            return;
        }
        if (!cargo_incomplete_) {
            // Widen before bit operations to avoid promotion to signed int.
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

    /// A null header consumes no cargo sequence number (1000-3535
    /// §§2.2, 2.3.1).
    void on_null_header() {}

    /// Clear compensation state when the hub leaves the bus to reset.
    void on_hub_absent() {
        cargo_incomplete_ = false;
        peek_sequences_.fill(0);
    }

    /// Whether the hub still holds a remainder after truncation.
    [[nodiscard]] bool cargo_incomplete() const { return cargo_incomplete_; }

   private:
    bool cargo_incomplete_ = false;

    // Header peeks consumed per channel, wrapping with the 8-bit sequence
    // field.
    std::array<std::uint8_t, pico_sh2::shtp::MAX_TRACKED_CHANNELS>
        peek_sequences_{};
};

}  // namespace pico_bno085::hal
