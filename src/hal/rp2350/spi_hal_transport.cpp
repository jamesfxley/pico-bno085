// SPDX-License-Identifier: MIT
#include "pico_bno085/hal/spi_hal_transport.hpp"

#include <hardware/platform_defs.h>
#include <hardware/spi.h>
#include <hardware/structs/io_bank0.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "hardware/gpio.h"
#include "pico_bno085/hal/detail/pin_mux.hpp"
#include "pico_bno085/hal/duplex_write.hpp"
#include "pico_bno085/hal/pins.hpp"
#include "pico_sh2/sh2/hal_transport.hpp"
#include "pico_sh2/shtp/channels.hpp"
#include "pico_sh2/shtp/header.hpp"
#include "pico_sh2/util/result.hpp"

namespace pico_bno085::hal {

namespace {

// Clocked out on reads; the hub ignores MOSI while it is streaming a cargo.
constexpr std::uint8_t READ_FILL = 0x00;

bool hintn_asserted(std::uint32_t pin) {
    return static_cast<int>(gpio_get(pin)) == 0;
}

void assert_csn(std::uint32_t pin) { gpio_put(pin, false); }

void deassert_csn(std::uint32_t pin) { gpio_put(pin, true); }

}  // namespace

void SpiHalTransport::end() {
    pending_length_ = 0;
    core_.end();
    // Return MOSI to SIO so an I2C transport can drive the shared SA0 strap.
    gpio_init(config_.miso_pin);
    gpio_init(config_.sck_pin);
    gpio_init(config_.mosi_pin);
    gpio_init(config_.csn_pin);
}

pico_sh2::Result<void> SpiHalTransport::init() {
    // Reject rates above the hub's limit before touching pins
    // (1000-3927 §6.5.2) (HR-4).
    if (config_.baud_hz > SPI_MAX_BAUD_HZ) {
        return pico_sh2::Sh2Error::INVALID_ARGUMENT;
    }
    // Reject unreachable or out-of-bank GPIOs before touching pins (HR-4).
    // CSN is a plain GPIO and needs only the bank bound.
    constexpr detail::MuxBounds BOUNDS{.gpio_count = NUM_BANK0_GPIOS};
    const std::uint32_t index = spi_get_index(config_.instance);
    if (!detail::spi_rx(config_.miso_pin, index, BOUNDS) ||
        !detail::spi_sck(config_.sck_pin, index, BOUNDS) ||
        !detail::spi_tx(config_.mosi_pin, index, BOUNDS) ||
        !detail::in_bank(config_.csn_pin, BOUNDS)) {
        return pico_sh2::Sh2Error::INVALID_ARGUMENT;
    }
    // Straps and NRST first (PS1:PS0 = 1:1 selects SPI), hub held in reset.
    core_.begin_straps();

    // The SDK's rounded rate can exceed the requested rate.
    if (spi_init(config_.instance, config_.baud_hz) > SPI_MAX_BAUD_HZ) {
        spi_deinit(config_.instance);
        return pico_sh2::Sh2Error::INVALID_ARGUMENT;
    }
    spi_set_format(config_.instance, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
    gpio_set_function(config_.miso_pin, GPIO_FUNC_SPI);
    gpio_set_function(config_.sck_pin, GPIO_FUNC_SPI);
    gpio_set_function(config_.mosi_pin, GPIO_FUNC_SPI);

    // GPIO CSN keeps header and body within one assertion.
    gpio_init(config_.csn_pin);
    gpio_set_dir(config_.csn_pin, GPIO_OUT != 0U);
    deassert_csn(config_.csn_pin);

    return core_.arm_interrupt();
}

bool SpiHalTransport::wake_and_wait() {
    const std::uint32_t wake = core_.wake_pin();
    gpio_put(wake, false);  // WAKE active low
    const std::uint64_t deadline =
        TransportCore::now_us() + config_.wake_timeout_us;
    while (!hintn_asserted(core_.config().hintn_pin)) {
        if (TransportCore::now_us() >= deadline) {
            gpio_put(wake, true);  // release WAKE on timeout
            return false;
        }
    }
    return true;
}

pico_sh2::Result<pico_sh2::ReceivedTransfer> SpiHalTransport::read_transfer(
    std::span<std::uint8_t> dst) {
    // A public override can receive a short span, although the session never
    // passes one. It is rejected before dst.data() is written, because
    // total - HEADER_SIZE would underflow for it.
    if (dst.size() < pico_sh2::shtp::HEADER_SIZE) {
        return pico_sh2::Sh2Error::INVALID_ARGUMENT;
    }
    if (pending_length_ != 0) {
        if (dst.size() < pending_length_) {
            return pico_sh2::Sh2Error::INVALID_ARGUMENT;
        }
        std::copy_n(pending_.begin(), pending_length_, dst.begin());
        const auto length = pending_length_;
        pending_length_ = 0;
        return pico_sh2::ReceivedTransfer{.length = length,
                                          .intn_edge_us = pending_edge_us_};
    }
    // Take the timestamp while clearing the latch (AR-2). A fresh edge during
    // this single transfer belongs to the next cargo (1000-3535 §2.6).
    // I2C must discard the assertion for its second transfer.
    const std::uint64_t edge_us = core_.consume().edge_us;

    assert_csn(config_.csn_pin);

    // The body follows the header within one CSN assertion.
    spi_read_blocking(config_.instance, READ_FILL, dst.data(),
                      pico_sh2::shtp::HEADER_SIZE);

    auto parsed = pico_sh2::shtp::parse_header(
        std::span(dst.data(), pico_sh2::shtp::HEADER_SIZE));
    if (!parsed.ok()) {
        // Pass the reserved dead-bus pattern to the engine so reset recovery
        // can run (1000-3535 §2.2) (SR-5).
        core_.note_absent_read();
        deassert_csn(config_.csn_pin);
        return pico_sh2::ReceivedTransfer{.length = pico_sh2::shtp::HEADER_SIZE,
                                          .intn_edge_us = edge_us};
    }
    if (parsed.value().transfer_length <= pico_sh2::shtp::HEADER_SIZE) {
        // A null header carries no cargo (1000-3535 §2.3.1).
        deassert_csn(config_.csn_pin);
        return pico_sh2::ReceivedTransfer{.length = pico_sh2::shtp::HEADER_SIZE,
                                          .intn_edge_us = edge_us};
    }

    const std::size_t length = parsed.value().transfer_length;
    const std::size_t total = length < dst.size() ? length : dst.size();
    const std::size_t body = total - pico_sh2::shtp::HEADER_SIZE;
    spi_read_blocking(config_.instance, READ_FILL,
                      dst.data() + pico_sh2::shtp::HEADER_SIZE, body);

    deassert_csn(config_.csn_pin);
    return pico_sh2::ReceivedTransfer{.length = total, .intn_edge_us = edge_us};
}

pico_sh2::Result<void> SpiHalTransport::write_transfer(
    std::span<const std::uint8_t> frame) {
    if (frame.size() < pico_sh2::shtp::HEADER_SIZE ||
        frame.size() > pending_.size()) {
        return pico_sh2::Sh2Error::INVALID_ARGUMENT;
    }
    if (pending_length_ != 0) {
        return pico_sh2::Sh2Error::BUSY;
    }
    if (!wake_and_wait()) {
        return pico_sh2::Sh2Error::TIMEOUT;
    }
    assert_csn(config_.csn_pin);
    gpio_put(core_.wake_pin(), true);
    // The answering edge belongs to this duplex transfer (1000-3535 §2.6)
    // (HR-4).
    const auto edge_us = core_.consume().edge_us;
    spi_write_read_blocking(config_.instance, frame.data(), pending_.data(),
                            pico_sh2::shtp::HEADER_SIZE);
    const auto plan = duplex_write(
        frame.size(), std::span<const std::uint8_t, 4>(pending_.data(), 4),
        pending_.size());
    // The longer direction sets the transfer length (1000-3535 §2.3.1) (HR-4).
    if (frame.size() > pico_sh2::shtp::HEADER_SIZE) {
        spi_write_read_blocking(config_.instance,
                                frame.data() + pico_sh2::shtp::HEADER_SIZE,
                                pending_.data() + pico_sh2::shtp::HEADER_SIZE,
                                frame.size() - pico_sh2::shtp::HEADER_SIZE);
    }
    if (plan.received > frame.size()) {
        spi_read_blocking(config_.instance, READ_FILL,
                          pending_.data() + frame.size(),
                          plan.received - frame.size());
    }
    deassert_csn(config_.csn_pin);
    if (plan.cargo) {
        pending_length_ = plan.received;
        pending_edge_us_ = edge_us;
    } else {
        core_.discard_continuation_assertion();
    }
    return {};
}

}  // namespace pico_bno085::hal
