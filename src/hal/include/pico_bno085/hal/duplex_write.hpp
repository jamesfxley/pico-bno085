// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace pico_bno085::hal {

/// Clocking plan for one simultaneous SPI read/write (HR-4).
struct DuplexWrite {
    std::size_t clocks;    ///< Total header and body clocks in bytes.
    std::size_t received;  ///< Inbound bytes retained within staging capacity.
    bool cargo;            ///< An inbound header announces a nonempty cargo.
    bool truncated;        ///< Inbound cargo exceeds staging capacity.
    bool continuation;     ///< Inbound header's continuation bit.
};

/// Plan padding per 1000-3535 2.3.1 and simultaneous exchange per 3.3.
/// \param frame outbound frame length, including its header.
/// \param header four inbound header bytes.
/// \param capacity available staging bytes, at least four.
/// \return byte counts and inbound-header classification.
[[nodiscard]] inline DuplexWrite duplex_write(
    std::size_t frame, std::span<const std::uint8_t, 4> header,
    std::size_t capacity) {
    const auto raw = static_cast<std::uint16_t>(
        static_cast<std::uint32_t>(header[0]) |
        (static_cast<std::uint32_t>(header[1]) << 8U));
    const std::size_t length = raw & 0x7FFFU;
    const bool cargo = raw != 0xFFFFU && length > 4;
    const std::size_t received = cargo ? std::min(length, capacity) : 4;
    return {.clocks = std::max(frame, received),
            .received = received,
            .cargo = cargo,
            .truncated = cargo && length > capacity,
            .continuation = (raw & 0x8000U) != 0};
}

}  // namespace pico_bno085::hal
