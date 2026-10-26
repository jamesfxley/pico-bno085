// SPDX-License-Identifier: MIT
// Drives the split-read compensation over arbitrary transfer streams.
//
// It rewrites two bytes of a header the framing engine then trusts, so a
// mistake here is a mistake the engine has no way to notice: a cargo dropped as
// an out-of-order fragment, or every cargo scored as a dropped one. The fuzzer
// feeds it whatever the hub could send, in any order, and checks the invariants
// that hold regardless of the bytes.
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

/// What a cargo may and may not do to the bytes it was handed.
///
/// \param before the transfer as the hub offered it.
/// \param after the transfer as the engine will read it.
/// \param followed_incomplete a cargo was left unfinished before this one, so
///        the continuation flag is the hub's own and must survive.
/// \param channel the SHTP channel the cargo arrived on.
/// \param first_since_absence no cargo has been retrieved on this channel since
///        the hub last left the bus, so exactly one peek has been consumed.
void check_cargo(std::span<const std::uint8_t> before,
                 std::span<const std::uint8_t> after, bool followed_incomplete,
                 std::uint8_t channel, bool first_since_absence) {
    if (before.size() < pico_sh2::shtp::HEADER_SIZE) {
        // Too short to hold the fields the correction addresses, so there is
        // nothing the compensator is entitled to touch.
        require(std::ranges::equal(before, after));
        return;
    }

    // Only the two header bytes named in HR-6 may move. The payload is what the
    // engine hands to the application, and a compensation that reached into it
    // would corrupt a report rather than a frame.
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (i == pico_bno085::hal::HEADER_LENGTH_MSB ||
            i == pico_bno085::hal::HEADER_SEQUENCE) {
            continue;
        }
        require(before[i] == after[i]);
    }

    const auto length_msb = pico_bno085::hal::HEADER_LENGTH_MSB;
    if (followed_incomplete) {
        // The hub is genuinely mid-cargo, so the flag it set is its own.
        require(before[length_msb] == after[length_msb]);
    } else {
        // Otherwise the flag describes this host's peek and the engine would
        // drop the transfer as an out-of-order fragment.
        require((static_cast<unsigned>(after[length_msb]) &
                 static_cast<unsigned>(CONTINUATION_IN_MSB)) == 0);
        // Nothing but that flag: the length itself is the hub's.
        require((static_cast<unsigned>(after[length_msb]) |
                 static_cast<unsigned>(CONTINUATION_IN_MSB)) ==
                (static_cast<unsigned>(before[length_msb]) |
                 static_cast<unsigned>(CONTINUATION_IN_MSB)));
    }

    const auto sequence = pico_bno085::hal::HEADER_SEQUENCE;
    if (channel >= pico_sh2::shtp::MAX_TRACKED_CHANNELS) {
        // No peek count is kept for a channel the engine does not track.
        require(before[sequence] == after[sequence]);
    } else if (first_since_absence) {
        // The hub restarts its sequence numbers while away, so the count
        // restarts with it: the first cargo after an absence has consumed
        // exactly one peek, however many idle polls preceded it.
        require(after[sequence] ==
                static_cast<std::uint8_t>(before[sequence] - 1U));
    }
}

}  // namespace

// The entry point's name is libFuzzer's, not ours.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    SplitReadCompensator compensator;
    std::vector<std::uint8_t> buffer(data, data + size);
    // Which channels have carried a cargo since the hub last left the bus.
    std::array<bool, pico_sh2::shtp::MAX_TRACKED_CHANNELS> seen{};

    std::size_t offset = 0;
    while (offset < buffer.size()) {
        // The first byte of each step decides what happened on the bus, so a
        // single input exercises interleavings of cargoes, idle polls and the
        // hub dropping off that no hand-written case would cover.
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
                // A length of its own, so the bytes after a cargo are the next
                // step rather than bytes the compensator has just rewritten.
                const std::size_t want = buffer[offset++];
                const std::size_t len = std::min(want, buffer.size() - offset);
                const bool truncated =
                    (static_cast<unsigned>(action) & 0x80U) != 0;
                // Lengths shorter than a header included, which on_cargo has to
                // leave alone.
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

    // Absence clears the state the hub abandoned, whatever preceded it.
    compensator.on_hub_absent();
    require(!compensator.cargo_incomplete());
    return 0;
}
