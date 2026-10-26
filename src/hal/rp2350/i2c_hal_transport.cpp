// SPDX-License-Identifier: MIT
#include "pico_bno085/hal/i2c_hal_transport.hpp"

#include <hardware/i2c.h>
#include <hardware/structs/io_bank0.h>
#include <pico/error.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "hardware/gpio.h"
#include "pico_bno085/hal/pins.hpp"
#include "pico_bno085/hal/split_read.hpp"
#include "pico_sh2/sh2/hal_transport.hpp"
#include "pico_sh2/shtp/channels.hpp"
#include "pico_sh2/shtp/header.hpp"
#include "pico_sh2/util/result.hpp"

namespace pico_bno085::hal {

// split_read.hpp restates these so it needs no SDK include and stays
// host-testable; a divergence must be a build failure here, not a surprise on
// the bench.
static_assert(SDK_ERROR_GENERIC == PICO_ERROR_GENERIC);
static_assert(SDK_ERROR_TIMEOUT == PICO_ERROR_TIMEOUT);

namespace {

// A transfer that failed for a reason other than the hub leaving the bus.
pico_sh2::Sh2Error bus_fault(TransferOutcome outcome) {
    return outcome == TransferOutcome::TIMEOUT ? pico_sh2::Sh2Error::TIMEOUT
                                               : pico_sh2::Sh2Error::BUS_ERROR;
}

}  // namespace

void I2cHalTransport::end() {
    core_.end();
    // Hand the pads back as SIO inputs. Leaving them assigned to the I2C
    // peripheral would mean a transport claiming them next drives a pad the
    // peripheral still owns, and the level never reaches the hub.
    gpio_init(config_.sda_pin);
    gpio_init(config_.scl_pin);
}

std::size_t I2cHalTransport::note_absent(std::span<std::uint8_t> dst) {
    core_.note_absent_read();
    split_read_.on_hub_absent();
    std::fill_n(dst.begin(), pico_sh2::shtp::HEADER_SIZE, std::uint8_t{0});
    return pico_sh2::shtp::HEADER_SIZE;
}

pico_sh2::Result<void> I2cHalTransport::init() {
    // The hub's limit, not a preference (1000-3927 §6.5.1), so a faster clock
    // is refused before any pin is touched (HR-4).
    if (config_.baud_hz > I2C_MAX_BAUD_HZ) {
        return pico_sh2::Sh2Error::INVALID_ARGUMENT;
    }
    // Straps and NRST first, hub held in reset until the session releases it.
    core_.begin_straps();

    // The SDK sets the nearest rate it can reach, which may lie above the
    // request, so the rate it reports is held to the limit too.
    if (i2c_init(config_.instance, config_.baud_hz) > I2C_MAX_BAUD_HZ) {
        i2c_deinit(config_.instance);
        return pico_sh2::Sh2Error::INVALID_ARGUMENT;
    }
    gpio_set_function(config_.sda_pin, GPIO_FUNC_I2C);
    gpio_set_function(config_.scl_pin, GPIO_FUNC_I2C);
    gpio_pull_up(config_.sda_pin);
    gpio_pull_up(config_.scl_pin);

    return core_.arm_interrupt();
}

pico_sh2::Result<pico_sh2::ReceivedTransfer> I2cHalTransport::read_transfer(
    std::span<std::uint8_t> dst) {
    // A span too short to hold a header cannot carry a transfer, and the reads
    // below write through dst.data() without consulting its size. The session
    // sizes rx_staging_ from the profile so it can never be short, but this is
    // a public override a consumer can call directly, so the precondition is
    // checked here rather than assumed of the caller (CXX-04.5).
    if (dst.size() < pico_sh2::shtp::HEADER_SIZE) {
        return pico_sh2::Sh2Error::INVALID_ARGUMENT;
    }
    // Read the header first to learn the cargo length, then re-read the whole
    // cargo (the hub re-offers it including the header). The split is not a
    // choice: the length is only knowable from the header, and a BNO085 asked
    // for more bytes than it holds stretches the clock until it has data
    // (1000-3927 §1.2.2.2), so there is no safe over-read to predict with.
    //
    // Consuming the pending assertion clears the latch and takes the edge this
    // read is anchored to (AR-2). It accounts for the header read alone; the
    // hub asserts once per transfer (1000-3535 §2.6), so the cargo read below
    // earns a second assertion that discard_continuation_assertion() accounts
    // for. The edge is the first assertion's, the one a segmented cargo is
    // timed from (1000-3535 §2.6).
    const std::uint64_t edge_us = core_.consume().edge_us;

    const int header = i2c_read_timeout_us(
        config_.instance, config_.address, dst.data(),
        pico_sh2::shtp::HEADER_SIZE, false, config_.transfer_timeout_us);
    const TransferOutcome header_outcome = classify(header);
    if (header_outcome != TransferOutcome::OK) {
        // A read is only issued behind a HINTN assertion (HR-1), so a hub that
        // does not acknowledge it has left the bus since asserting — it is
        // resetting, not faulty. Report the zero-length transfer a hub that
        // cannot answer produces, which is all a transport without an
        // acknowledge bit can report, and let the session's reset recovery
        // (pico-sh2 SR-5) take it from there.
        if (header_outcome == TransferOutcome::HUB_ABSENT) {
            return pico_sh2::ReceivedTransfer{.length = note_absent(dst),
                                              .intn_edge_us = edge_us};
        }
        return bus_fault(header_outcome);
    }
    if (std::cmp_not_equal(header, pico_sh2::shtp::HEADER_SIZE)) {
        return pico_sh2::Sh2Error::BUS_ERROR;
    }

    // Dead bus (0xFFFF) or a header-only / zero-length poll (BNO086 quirk):
    // hand the four bytes to the engine, which treats them as empty or rejects
    // them. Never an error — that would fault an otherwise healthy session.
    auto parsed = pico_sh2::shtp::parse_header(
        std::span(dst.data(), pico_sh2::shtp::HEADER_SIZE));
    if (!parsed.ok() ||
        parsed.value().transfer_length <= pico_sh2::shtp::HEADER_SIZE) {
        split_read_.on_null_header();
        return pico_sh2::ReceivedTransfer{.length = pico_sh2::shtp::HEADER_SIZE,
                                          .intn_edge_us = edge_us};
    }

    const std::size_t length = parsed.value().transfer_length;
    const bool truncated = length > dst.size();
    const std::size_t to_read = truncated ? dst.size() : length;
    const int body =
        i2c_read_timeout_us(config_.instance, config_.address, dst.data(),
                            to_read, false, config_.transfer_timeout_us);
    const TransferOutcome body_outcome = classify(body);
    if (body_outcome != TransferOutcome::OK) {
        // The hub left the bus between offering the header and serving the
        // cargo; the partial read in dst describes nothing.
        if (body_outcome == TransferOutcome::HUB_ABSENT) {
            return pico_sh2::ReceivedTransfer{.length = note_absent(dst),
                                              .intn_edge_us = edge_us};
        }
        return bus_fault(body_outcome);
    }
    if (std::cmp_not_equal(body, to_read)) {
        return pico_sh2::Sh2Error::BUS_ERROR;
    }

    // Two transfers were performed, so the hub asserted twice. The second
    // assertion announces the read that just completed, not a cargo (HR-6).
    core_.discard_continuation_assertion();

    split_read_.on_cargo(dst.first(to_read), parsed.value().channel, truncated);
    return pico_sh2::ReceivedTransfer{.length = to_read,
                                      .intn_edge_us = edge_us};
}

pico_sh2::Result<void> I2cHalTransport::write_transfer(
    std::span<const std::uint8_t> frame) {
    // The session frames every write to fit one transfer; no wake handshake on
    // I2C. The hub cannot reassemble, so a short write is a bus fault.
    const int written =
        i2c_write_timeout_us(config_.instance, config_.address, frame.data(),
                             frame.size(), false, config_.transfer_timeout_us);
    const TransferOutcome outcome = classify(written);
    if (outcome != TransferOutcome::OK) {
        // A write is host-initiated, so an unacknowledged address is not the
        // benign "hub is resetting" case a read's is: the session asked for
        // something the hub never received. Surface it.
        return bus_fault(outcome);
    }
    if (static_cast<std::size_t>(written) != frame.size()) {
        return pico_sh2::Sh2Error::BUS_ERROR;
    }
    return {};
}

}  // namespace pico_bno085::hal
