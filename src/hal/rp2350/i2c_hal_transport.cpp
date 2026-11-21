// SPDX-License-Identifier: MIT
#include "pico_bno085/hal/i2c_hal_transport.hpp"

#include <hardware/i2c.h>
#include <hardware/platform_defs.h>
#include <hardware/structs/io_bank0.h>
#include <pico/error.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

#include "hardware/gpio.h"
#include "pico_bno085/hal/detail/pin_mux.hpp"
#include "pico_bno085/hal/pins.hpp"
#include "pico_bno085/hal/split_read.hpp"
#include "pico_sh2/sh2/hal_transport.hpp"
#include "pico_sh2/shtp/channels.hpp"
#include "pico_sh2/shtp/header.hpp"
#include "pico_sh2/util/result.hpp"

namespace pico_bno085::hal {

// A mismatch with the SDK codes must fail compilation.
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
    // Return pads to SIO inputs so the next transport can drive them.
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
    // Reject rates above the hub's limit before touching pins
    // (1000-3927 §6.5.1) (HR-4).
    if (config_.baud_hz > I2C_MAX_BAUD_HZ) {
        return pico_sh2::Sh2Error::INVALID_ARGUMENT;
    }
    // Reject unreachable or out-of-bank GPIOs before touching pins (HR-4).
    constexpr detail::MuxBounds BOUNDS{.gpio_count = NUM_BANK0_GPIOS};
    const std::uint32_t index = i2c_get_index(config_.instance);
    if (!detail::i2c_sda(config_.sda_pin, index, BOUNDS) ||
        !detail::i2c_scl(config_.scl_pin, index, BOUNDS) ||
        !detail::in_bank(config_.control.hintn_pin, BOUNDS)) {
        return pico_sh2::Sh2Error::INVALID_ARGUMENT;
    }
    // Straps and NRST first, hub held in reset until the session releases it.
    core_.begin_straps();

    // The SDK's rounded rate can exceed the requested rate.
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
    // A public override can receive a short span, although the session never
    // passes one, and the reads below write through dst.data() unchecked.
    if (dst.size() < pico_sh2::shtp::HEADER_SIZE) {
        return pico_sh2::Sh2Error::INVALID_ARGUMENT;
    }
    // Take the timestamp while clearing the latch (AR-2). The first assertion
    // anchors a segmented cargo (1000-3535 §2.6).
    const std::uint64_t edge_us = core_.consume().edge_us;

    const int header = i2c_read_timeout_us(
        config_.instance, config_.address, dst.data(),
        pico_sh2::shtp::HEADER_SIZE, false, config_.transfer_timeout_us);
    const TransferOutcome header_outcome = classify(header);
    if (header_outcome != TransferOutcome::OK) {
        // Reads start only behind HINTN (HR-1), so a hub that does not
        // acknowledge has left the bus to reset. Report it as an empty transfer
        // so the session's reset recovery (pico-sh2 SR-5) can run.
        if (header_outcome == TransferOutcome::HUB_ABSENT) {
            return pico_sh2::ReceivedTransfer{.length = note_absent(dst),
                                              .intn_edge_us = edge_us};
        }
        return bus_fault(header_outcome);
    }
    if (std::cmp_not_equal(header, pico_sh2::shtp::HEADER_SIZE)) {
        return pico_sh2::Sh2Error::BUS_ERROR;
    }

    // Pass empty or invalid headers to the engine without a transport error.
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
        // An absent hub invalidates the partially read cargo.
        if (body_outcome == TransferOutcome::HUB_ABSENT) {
            return pico_sh2::ReceivedTransfer{.length = note_absent(dst),
                                              .intn_edge_us = edge_us};
        }
        return bus_fault(body_outcome);
    }
    if (std::cmp_not_equal(body, to_read)) {
        return pico_sh2::Sh2Error::BUS_ERROR;
    }

    // Clear the assertion for the completed second transfer (HR-6).
    core_.discard_continuation_assertion();

    split_read_.on_cargo(dst.first(to_read), parsed.value().channel, truncated);
    return pico_sh2::ReceivedTransfer{.length = to_read,
                                      .intn_edge_us = edge_us};
}

pico_sh2::Result<void> I2cHalTransport::write_transfer(
    std::span<const std::uint8_t> frame) {
    // A short write is a fault because the hub cannot reassemble it.
    const int written =
        i2c_write_timeout_us(config_.instance, config_.address, frame.data(),
                             frame.size(), false, config_.transfer_timeout_us);
    const TransferOutcome outcome = classify(written);
    if (outcome != TransferOutcome::OK) {
        // A host-initiated write that the hub never received is a fault.
        return bus_fault(outcome);
    }
    if (static_cast<std::size_t>(written) != frame.size()) {
        return pico_sh2::Sh2Error::BUS_ERROR;
    }
    return {};
}

}  // namespace pico_bno085::hal
