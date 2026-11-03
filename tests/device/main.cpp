// SPDX-License-Identifier: MIT
// Device-only reset and shared-wiring bring-up paths (HR-3, AR-3).
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <system_error>

#include "hardware/timer.h"
#include "pico/stdio.h"
#include "pico/time.h"
#include "pico/types.h"
#include "pico_bno085/driver/bno085.hpp"
#include "pico_bno085/hal/i2c_hal_transport.hpp"
#include "pico_bno085/hal/spi_hal_transport.hpp"
#include "pico_family/device_test.hpp"
#include "pico_sh2/reports/sensor_event.hpp"
#include "pico_sh2/reports/sensor_id.hpp"

namespace {

using pico_sh2::reports::SensorEvent;
using pico_sh2::reports::SensorId;

constexpr std::uint32_t GUARD_MS = 3000;
constexpr std::uint32_t REPORT_INTERVAL_US = 20'000;
// Leave room before the image's 20 s limit for a timeout-bounded service call.
constexpr std::uint32_t RUN_GUARD_MS = 14'000;

// Typed integer formatting keeps diagnostics within static storage.
struct FailureText {
    std::array<char, 512> bytes{};
    std::size_t size = 0;
    bool complete = true;

    void append(std::string_view text) {
        if (text.size() > bytes.size() - size) {
            complete = false;
            return;
        }
        for (const char ch : text) {
            bytes.at(size) = ch;
            ++size;
        }
    }

    void field(std::string_view label, std::int64_t value) {
        append(label);
        const auto remaining = std::span(bytes).subspan(size);
        const auto converted =
            std::to_chars(remaining.data(), remaining.last<0>().data(), value);
        if (converted.ec != std::errc{}) {
            complete = false;
            return;
        }
        size += static_cast<std::size_t>(converted.ptr - remaining.data());
    }
};

// Session buffers and IRQ-registered transports outlive every case frame.
// Only one transport owns the shared wiring at a time.
struct DeviceState {
    pico_bno085::hal::I2cHalTransport i2c{{}};
    pico_bno085::hal::SpiHalTransport spi{{}};
    pico_bno085::Bno085 i2c_imu{i2c};
    pico_bno085::Bno085 spi_imu{spi};
    absolute_time_t run_deadline{};
    // The event sink runs synchronously inside service(), never in an IRQ.
    bool sensor_event = false;
    FailureText failure_text;
    // Refers to failure_text or a stage string literal; both outlive Run.
    std::string_view failure_detail;
};

void on_event(void* context, const SensorEvent& event) {
    if (event.sensor == SensorId::ROTATION_VECTOR) {
        static_cast<DeviceState*>(context)->sensor_event = true;
    }
}

template <typename Predicate>
bool service_until(const DeviceState& state, pico_bno085::Bno085& imu,
                   absolute_time_t deadline, Predicate satisfied) {
    while (!time_reached(deadline) && !time_reached(state.run_deadline)) {
        imu.service();
        if (imu.faulted()) {
            return false;
        }
        if (satisfied()) {
            return !time_reached(deadline) && !time_reached(state.run_deadline);
        }
        sleep_ms(1);
    }
    return false;
}

template <typename Transport>
bool default_boot(const DeviceState& state, Transport& transport,
                  pico_bno085::Bno085& imu) {
    const auto deadline = make_timeout_time_ms(GUARD_MS);
    const bool initialized = transport.init().ok();
    bool ready = false;
    if (initialized) {
        imu.begin();
        ready =
            service_until(state, imu, deadline, [&imu] { return imu.ready(); });
    }
    transport.end();
    return initialized && ready;
}

// The fixed labels, stage literals and bounded integer fields fit in 512 bytes.
// Only one failure is formatted per case; the buffer begins empty.
bool fail_reset_case(DeviceState& state, std::string_view stage,
                     const pico_bno085::Stats& before, int error = 0) {
    const auto after = state.i2c_imu.stats();
    auto& text = state.failure_text;
    text.append(stage);
    text.field(": error=", error);
    text.field(" ready=", static_cast<int>(state.i2c_imu.ready()));
    text.field(" state=", static_cast<int>(state.i2c_imu.state()));
    text.field(" phase=", static_cast<int>(state.i2c_imu.startup_phase()));
    text.field(" faulted=", static_cast<int>(state.i2c_imu.faulted()));
    text.field(" fault=", static_cast<int>(state.i2c_imu.fault()));
    text.field(" event=", static_cast<int>(state.sensor_event));
    text.field(" enabled=", after.enabled_sensors);
    text.field(" hub_resets=", before.hub_resets);
    text.field("->", after.hub_resets);
    text.field(" recoveries=", before.recoveries);
    text.field("->", after.recoveries);
    text.field(" sensors_reenabled=", before.sensors_reenabled);
    text.field("->", after.sensors_reenabled);
    text.field(" run_guard_expired=",
               static_cast<int>(time_reached(state.run_deadline)));
    // Preserve the failing stage even if diagnostic formatting itself fails.
    state.failure_detail =
        text.complete ? std::string_view(text.bytes.data(), text.size) : stage;
    return false;
}

bool reset_restores_sensor(DeviceState& state) {
    const auto initial = state.i2c_imu.stats();
    const auto boot_deadline = make_timeout_time_ms(GUARD_MS);
    const auto initialized = state.i2c.init();
    if (!initialized.ok()) {
        return fail_reset_case(state, "i2c init", initial,
                               static_cast<int>(initialized.error()));
    }
    state.i2c_imu.set_event_sink({&on_event, &state});
    state.i2c_imu.begin();
    if (!service_until(state, state.i2c_imu, boot_deadline,
                       [&state] { return state.i2c_imu.ready(); })) {
        return fail_reset_case(state, "startup not READY", initial);
    }
    state.sensor_event = false;
    const auto enabled = state.i2c_imu.enable_sensor(
        SensorId::ROTATION_VECTOR, {.report_interval_us = REPORT_INTERVAL_US});
    if (!enabled.ok()) {
        return fail_reset_case(state, "enable refused", initial,
                               static_cast<int>(enabled.error()));
    }
    if (!service_until(state, state.i2c_imu, make_timeout_time_ms(GUARD_MS),
                       [&state] { return state.sensor_event; })) {
        return fail_reset_case(state, "no event before reset", initial);
    }

    const auto before = state.i2c_imu.stats();
    const auto reset_deadline = make_timeout_time_ms(GUARD_MS);
    const auto reset = state.i2c_imu.reset();
    if (!reset.ok()) {
        return fail_reset_case(state, "reset refused", before,
                               static_cast<int>(reset.error()));
    }
    // A queued reset leaves READY true until the hub answers. Require its
    // reset notification and re-apply, including a handshake drained in one
    // service() call. A pre-reset event cannot satisfy the stream check.
    if (!service_until(state, state.i2c_imu, reset_deadline, [&state, &before] {
            const auto after = state.i2c_imu.stats();
            return state.i2c_imu.ready() &&
                   after.hub_resets > before.hub_resets &&
                   after.recoveries > before.recoveries &&
                   after.sensors_reenabled > before.sensors_reenabled;
        })) {
        return fail_reset_case(state, "recovery", before);
    }
    state.sensor_event = false;
    if (!service_until(state, state.i2c_imu, reset_deadline, [&state] {
            return state.sensor_event && state.i2c_imu.ready();
        })) {
        return fail_reset_case(state, "no event after reset", before);
    }
    return true;
}

}  // namespace

int main() {
    stdio_init_all();
    pico_family::device_test::Run run;
    static DeviceState state;
    state.run_deadline = make_timeout_time_ms(RUN_GUARD_MS);

    const bool i2c_ready = default_boot(state, state.i2c, state.i2c_imu);
    const bool spi_ready = default_boot(state, state.spi, state.spi_imu);
    const char* boot_detail = "";
    if (!i2c_ready) {
        boot_detail = "I2C bring-up failed";
    } else if (!spi_ready) {
        boot_detail = "SPI bring-up failed";
    }
    run.check("HR-3", "default-straps-boot-each-bus", i2c_ready && spi_ready,
              boot_detail);

    const bool restored = reset_restores_sensor(state);
    state.i2c.end();
    run.check("AR-3", "hub-reset-restores-desired-sensor", restored,
              restored ? std::string_view{} : state.failure_detail);
    run.finish();
}
