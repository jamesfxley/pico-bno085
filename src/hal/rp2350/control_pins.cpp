// SPDX-License-Identifier: MIT
#include "pico_bno085/hal/control_pins.hpp"

#include <cstddef>
#include <cstdint>

#include "hardware/gpio.h"
#include "pico_bno085/hal/detail/strap_plan.hpp"

namespace pico_bno085::hal {

namespace {

void drive_output(std::uint32_t pin, bool high) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_OUT != 0U);
    gpio_put(pin, high);
}

}  // namespace

void ControlPins::init() {
    // NRST is asserted before the straps change (HR-3).
    const detail::StrapPlan plan =
        detail::strap_plan(detail::StrapPins{.nrst = config_.nrst_pin,
                                             .ps0 = config_.ps0_pin,
                                             .ps1 = config_.ps1_pin,
                                             .bootn = config_.bootn_pin,
                                             .sa0 = config_.sa0_pin},
                           bus_ == Bus::SPI, sa0_high_);
    for (std::size_t i = 0; i < plan.count; ++i) {
        drive_output(plan.steps.at(i).pin, plan.steps.at(i).high);
    }
}

void ControlPins::set_reset(bool asserted) const {
    gpio_put(config_.nrst_pin, !asserted);
}

}  // namespace pico_bno085::hal
