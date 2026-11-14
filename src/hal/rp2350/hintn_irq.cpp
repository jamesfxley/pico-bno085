// SPDX-License-Identifier: MIT
#include "pico_bno085/hal/hintn_irq.hpp"

#include <hardware/irq.h>
#include <hardware/regs/intctrl.h>
#include <hardware/timer.h>
#include <pico/types.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "hardware/gpio.h"
#include "pico_sh2/util/result.hpp"

namespace pico_bno085::hal {

namespace {

// One entry per concurrently wired hub; a single IMU needs one.
constexpr std::size_t MAX_LATCHES = 2;

// Raw GPIO handlers leave the consumer's default callback available.
// Keep the registered mask: SDK removal needs the original mask, and
// registering the same pin twice asserts (hardware/gpio.h).
struct DispatchTable {
    std::array<HintnLatch*, MAX_LATCHES> latches{};
    std::uint32_t raw_mask = 0;
};

// cppcoreguidelines-avoid-non-const-global-variables: the SDK handler takes
// no context argument, so its dispatch table needs file scope.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DispatchTable g_dispatch;

// A raw handler acknowledges only the pins it owns (hardware/gpio.h).
void dispatch() {
    const std::uint64_t edge_us = time_us_64();
    for (HintnLatch* latch : g_dispatch.latches) {
        if (latch == nullptr) {
            continue;
        }
        const uint gpio = latch->pin();
        if ((gpio_get_irq_event_mask(gpio) & GPIO_IRQ_EDGE_FALL) == 0) {
            continue;
        }
        gpio_acknowledge_irq(gpio, GPIO_IRQ_EDGE_FALL);
        // Drops an edge whose line has already risen (HR-1).
        latch->latch(edge_us);
    }
}

std::uint32_t latched_pin_mask() {
    std::uint32_t mask = 0;
    for (const HintnLatch* latch : g_dispatch.latches) {
        if (latch != nullptr) {
            mask |= 1U << latch->pin();
        }
    }
    return mask;
}

// Update the registered mask only when it changes to avoid duplicate claims.
void install_dispatch(std::uint32_t mask) {
    if (mask == g_dispatch.raw_mask) {
        return;
    }
    // Disable the bank while changing handlers so no edge arrives without one.
    const bool was_enabled = irq_is_enabled(IO_IRQ_BANK0);
    irq_set_enabled(IO_IRQ_BANK0, false);
    if (g_dispatch.raw_mask != 0) {
        gpio_remove_raw_irq_handler_masked(g_dispatch.raw_mask, &dispatch);
    }
    if (mask != 0) {
        gpio_add_raw_irq_handler_masked(mask, &dispatch);
    }
    g_dispatch.raw_mask = mask;
    irq_set_enabled(IO_IRQ_BANK0, was_enabled);
}

// Reuse an existing registration so reinitialisation consumes no new slot.
bool register_latch(HintnLatch* latch) {
    for (HintnLatch* slot : g_dispatch.latches) {
        if (slot == latch) {
            return true;
        }
    }
    for (HintnLatch*& slot : g_dispatch.latches) {
        if (slot == nullptr) {
            slot = latch;
            return true;
        }
    }
    return false;
}

void unregister_latch(const HintnLatch* latch) {
    for (HintnLatch*& slot : g_dispatch.latches) {
        if (slot == latch) {
            slot = nullptr;
        }
    }
}

}  // namespace

pico_sh2::Result<void> HintnLatch::begin(std::uint32_t pin) {
    if (!register_latch(this)) {
        return pico_sh2::Sh2Error::TABLE_FULL;
    }
    pin_ = pin;
    begun_ = true;
    edge_.reset();

    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN != 0U);
    gpio_pull_up(pin);  // HINTN idles high, asserts low
    // Install before enabling: gpio_set_irq_enabled asserts without a handler.
    install_dispatch(latched_pin_mask());
    gpio_set_irq_enabled(pin, GPIO_IRQ_EDGE_FALL, true);
    irq_set_enabled(IO_IRQ_BANK0, true);
    return {};
}

void HintnLatch::end() {
    if (!begun_) {
        return;
    }
    gpio_set_irq_enabled(pin_, GPIO_IRQ_EDGE_FALL, false);
    unregister_latch(this);
    install_dispatch(latched_pin_mask());
    begun_ = false;
}

HintnLatch::~HintnLatch() { end(); }

bool HintnLatch::asserted() const {
    return static_cast<int>(gpio_get(pin_)) == 0;
}

}  // namespace pico_bno085::hal
