// SPDX-License-Identifier: MIT
#include "pico_bno085/hal/spi_hal_transport.hpp"

#include <hardware/spi.h>
#include <hardware/structs/io_bank0.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

#include "hardware/gpio.h"
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
    // Hand the pads back as SIO inputs. MOSI matters most: it is the same pad
    // as SA0, so an I2C transport claiming it next has to be able to drive the
    // address strap through SIO rather than through the SPI peripheral.
    gpio_init(config_.miso_pin);
    gpio_init(config_.sck_pin);
    gpio_init(config_.mosi_pin);
    gpio_init(config_.csn_pin);
}

pico_sh2::Result<void> SpiHalTransport::init() {
    // The hub's limit, not a preference (1000-3927 §6.5.2), so a faster clock
    // is refused before any pin is touched (HR-4).
    if (config_.baud_hz > SPI_MAX_BAUD_HZ) {
        return pico_sh2::Sh2Error::INVALID_ARGUMENT;
    }
    // Straps and NRST first (PS1:PS0 = 1:1 selects SPI), hub held in reset.
    core_.begin_straps();

    // The SDK sets the nearest rate it can reach, which may lie above the
    // request, so the rate it reports is held to the limit too.
    if (spi_init(config_.instance, config_.baud_hz) > SPI_MAX_BAUD_HZ) {
        spi_deinit(config_.instance);
        return pico_sh2::Sh2Error::INVALID_ARGUMENT;
    }
    spi_set_format(config_.instance, 8, SPI_CPOL_1, SPI_CPHA_1, SPI_MSB_FIRST);
    gpio_set_function(config_.miso_pin, GPIO_FUNC_SPI);
    gpio_set_function(config_.sck_pin, GPIO_FUNC_SPI);
    gpio_set_function(config_.mosi_pin, GPIO_FUNC_SPI);

    // CSN is a plain GPIO so a single transfer can span header and body; idle
    // high (deasserted).
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
    // A span too short to hold a header cannot carry a transfer. Checked before
    // anything is written through dst.data(): the body length below is
    // total - HEADER_SIZE, which underflows to a near-SIZE_MAX read for a
    // shorter span. The session sizes rx_staging_ from the profile so it can
    // never be short, but this is a public override a consumer can call
    // directly (CXX-04.5).
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
    // Consuming the pending assertion clears the latch and takes the edge this
    // read is anchored to (AR-2). One CSN assertion is one transfer, and the
    // hub asserts once per transfer (1000-3535 §2.6), so this read accounts for
    // the whole cargo and a fresh edge during it belongs to the next one —
    // unlike I2C, which retrieves a cargo in two transfers and must discard the
    // assertion the second earns.
    const std::uint64_t edge_us = core_.consume().edge_us;

    assert_csn(config_.csn_pin);

    // One CSN assertion streams header then body — the hub does not re-send the
    // header (unlike I2C); the payload follows the four header bytes directly.
    spi_read_blocking(config_.instance, READ_FILL, dst.data(),
                      pico_sh2::shtp::HEADER_SIZE);

    auto parsed = pico_sh2::shtp::parse_header(
        std::span(dst.data(), pico_sh2::shtp::HEADER_SIZE));
    if (!parsed.ok()) {
        // A hub that is not driving MISO leaves the line at the idle level, so
        // the header reads back as the reserved 0xFFFF that 1000-3535 §2.2 sets
        // aside for exactly this. SPI has no acknowledge bit, so this is the
        // only sign the hub has left the bus; count it as the absent read it is
        // and hand the bytes on, since a read error would fault a session that
        // is about to recover on the re-advertisement (SR-5).
        core_.note_absent_read();
        deassert_csn(config_.csn_pin);
        return pico_sh2::ReceivedTransfer{.length = pico_sh2::shtp::HEADER_SIZE,
                                          .intn_edge_us = edge_us};
    }
    if (parsed.value().transfer_length <= pico_sh2::shtp::HEADER_SIZE) {
        // A null header: the hub has nothing pending. Normal on SPI, where
        // every read clocks out a full transfer whether or not a cargo exists
        // (1000-3535 §2.3.1).
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
    // HR-4: the answering edge belongs to this duplex transfer (2.6).
    const auto edge_us = core_.consume().edge_us;
    spi_write_read_blocking(config_.instance, frame.data(), pending_.data(),
                            pico_sh2::shtp::HEADER_SIZE);
    const auto plan = duplex_write(
        frame.size(), std::span<const std::uint8_t, 4>(pending_.data(), 4),
        pending_.size());
    // Exchange the remaining frame, retaining the part within staging capacity.
    // HR-4, 1000-3535 2.3.1: the longer direction determines transfer length.
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
