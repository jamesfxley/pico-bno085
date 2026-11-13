// SPDX-License-Identifier: MIT
// Fuzz split-read compensation independently of the framing engine.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "pico_bno085/hal/split_read.hpp"
#include "pico_sh2/shtp/channels.hpp"

namespace {

using pico_bno085::hal::SplitReadCompensator;

void require(bool held) {
    if (!held) {
        __builtin_trap();
    }
}

/// The continuation flag as it sits in the length field's high byte.
constexpr auto CONTINUATION_IN_MSB = static_cast<std::uint8_t>(
    static_cast<unsigned>(pico_sh2::shtp::CONTINUATION_MASK) >> 8U);

/// Check what compensation may and may not change.
/// \param followed_incomplete whether the continuation flag is the hub's own
/// and must survive.
/// \param first_since_absence whether only one peek has occurred on this
/// channel.
void check_cargo(std::span<const std::uint8_t> before,
                 std::span<const std::uint8_t> after, bool followed_incomplete,
                 std::uint8_t channel, bool first_since_absence) {
    if (before.size() < pico_sh2::shtp::HEADER_SIZE) {
        // A short transfer contains no fields compensation can change.
        require(std::ranges::equal(before, after));
        return;
    }

    // Only the two compensation header bytes may change (HR-6).
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (i == pico_bno085::hal::HEADER_LENGTH_MSB ||
            i == pico_bno085::hal::HEADER_SEQUENCE) {
            continue;
        }
        require(before[i] == after[i]);
    }

    const auto length_msb = pico_bno085::hal::HEADER_LENGTH_MSB;
    if (followed_incomplete) {
        require(before[length_msb] == after[length_msb]);
    } else {
        // A peek's continuation flag would otherwise look like a fragment.
        require((static_cast<unsigned>(after[length_msb]) &
                 static_cast<unsigned>(CONTINUATION_IN_MSB)) == 0);
        require((static_cast<unsigned>(after[length_msb]) |
                 static_cast<unsigned>(CONTINUATION_IN_MSB)) ==
                (static_cast<unsigned>(before[length_msb]) |
                 static_cast<unsigned>(CONTINUATION_IN_MSB)));
    }

    const auto sequence = pico_bno085::hal::HEADER_SEQUENCE;
    if (channel >= pico_sh2::shtp::MAX_TRACKED_CHANNELS) {
        require(before[sequence] == after[sequence]);
    } else if (first_since_absence) {
        // The first cargo after absence consumes one peek, regardless of idle
        // polls.
        require(after[sequence] ==
                static_cast<std::uint8_t>(before[sequence] - 1U));
    }
}

}  // namespace

// readability-identifier-naming: libFuzzer requires this entry point's name.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    SplitReadCompensator compensator;
    std::vector<std::uint8_t> buffer(data, data + size);
    std::array<bool, pico_sh2::shtp::MAX_TRACKED_CHANNELS> seen{};

    std::size_t offset = 0;
    while (offset < buffer.size()) {
        // Each step selects cargo, idle poll or hub absence to vary
        // interleavings.
        const std::uint8_t action = buffer[offset++];
        switch (action % 4) {
            case 0:
                compensator.on_null_header();
                break;
            case 1:
                compensator.on_hub_absent();
                seen.fill(false);
                break;
            default: {
                if (buffer.size() - offset < 2) {
                    return 0;
                }
                const std::uint8_t channel = buffer[offset++];
                // A separate length keeps rewritten cargo bytes out of the next
                // step.
                const std::size_t want = buffer[offset++];
                const std::size_t len = std::min(want, buffer.size() - offset);
                const bool truncated =
                    (static_cast<unsigned>(action) & 0x80U) != 0;
                // Include short transfers, which on_cargo leaves unchanged.
                const std::span<std::uint8_t> transfer(buffer.data() + offset,
                                                       len);
                const std::vector<std::uint8_t> before(transfer.begin(),
                                                       transfer.end());
                const bool followed_incomplete = compensator.cargo_incomplete();
                const bool tracked =
                    channel < pico_sh2::shtp::MAX_TRACKED_CHANNELS;
                const bool first_since_absence = tracked && !seen[channel];

                compensator.on_cargo(transfer, channel, truncated);

                check_cargo(before, transfer, followed_incomplete, channel,
                            first_since_absence);
                if (tracked && len >= pico_sh2::shtp::HEADER_SIZE) {
                    seen[channel] = true;
                }
                offset += len;
                break;
            }
        }
    }

    compensator.on_hub_absent();
    require(!compensator.cargo_incomplete());
    return 0;
}
