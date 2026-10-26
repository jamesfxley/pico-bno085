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

// The latches this core dispatches to, and the GPIO mask currently registered
// for them.
//
// A raw handler rather than gpio_set_irq_enabled_with_callback: that call takes
// the core's single GPIO callback slot, so a consumer arming any other GPIO
// interrupt would silently replace this one -- reads would continue on the
// level fallback while every edge timestamp stopped, anchoring the whole
// system's events to zero. The raw mechanism claims only the pins named in the
// mask and leaves the default callback to the consumer (hardware/gpio.h).
//
// The mask lives beside the table because the SDK hard-asserts on a pin claimed
// twice and removes by the mask that was added, so what is registered has to be
// known exactly rather than recomputed from the table after it has changed.
struct DispatchTable {
    std::array<HintnLatch*, MAX_LATCHES> latches{};
    std::uint32_t raw_mask = 0;
};

// The GPIO handler the SDK invokes takes no context argument, so the table it
// dispatches through has to be reachable from file scope.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
DispatchTable g_dispatch;

// A raw handler is shared across every GPIO on the core, so it identifies and
// acknowledges its own pins (hardware/gpio.h).
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

// The pins the registered latches occupy.
std::uint32_t latched_pin_mask() {
    std::uint32_t mask = 0;
    for (const HintnLatch* latch : g_dispatch.latches) {
        if (latch != nullptr) {
            mask |= 1U << latch->pin();
        }
    }
    return mask;
}

// Bring the registered mask to `mask`, adding, moving or removing the handler
// as the table requires. A begin() that changes nothing registers nothing,
// which is what keeps re-initialising a transport from tripping the SDK's
// duplicate-claim assert.
void install_dispatch(std::uint32_t mask) {
    if (mask == g_dispatch.raw_mask) {
        return;
    }
    // Bounded and init-time: the window spans two SDK calls, and leaving the
    // bank enabled across a mask change would let an edge arrive with no
    // handler claiming it.
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

// Claim a dispatch slot. Already-registered latches keep the one they hold, so
// re-initialising a transport does not exhaust the table.
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
    // Ahead of enabling the interrupt: gpio_set_irq_enabled asserts that a
    // handler is already installed.
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
