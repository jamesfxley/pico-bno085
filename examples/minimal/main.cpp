// SPDX-License-Identifier: MIT
// I2C rotation-vector example. Bno085 reapplies desired sensors after reset.
#include <pico/stdio.h>
#include <pico/time.h>

#include <cstdint>
#include <variant>

#include "pico_bno085/driver/bno085.hpp"
#include "pico_bno085/hal/i2c_hal_transport.hpp"
#include "pico_logging/logging/logging.hpp"
#include "pico_sh2/reports/decoders.hpp"
#include "pico_sh2/reports/sensor_event.hpp"
#include "pico_sh2/reports/sensor_id.hpp"

namespace {

using pico_sh2::reports::RotationSample;
using pico_sh2::reports::SensorEvent;
using pico_sh2::reports::SensorId;

constexpr std::uint32_t ROTATION_VECTOR_INTERVAL_US = 20'000;  // 50 Hz

void on_event(void* /*ctx*/, const SensorEvent& event) {
    const auto* rotation = std::get_if<RotationSample>(&event.payload);
    if (rotation == nullptr) {
        return;
    }
    const pico_sh2::reports::Quaternion& quat = rotation->unit;
    PICO_LOG_INFO << "rv i=" << quat.i << " j=" << quat.j << " k=" << quat.k
                  << " r=" << quat.real;
}

// Static storage keeps session buffers off the 4 KiB core-0 stack.
pico_bno085::hal::I2cHalTransport g_transport{{}};
pico_bno085::Bno085 g_imu(g_transport);

}  // namespace

int main() {
    stdio_init_all();

    pico_bno085::hal::I2cHalTransport& transport = g_transport;
    if (!transport.init().ok()) {
        PICO_LOG_ERROR << "transport init failed";
        return 1;
    }

    pico_bno085::Bno085& imu = g_imu;
    imu.set_event_sink(pico_sh2::reports::EventSink(&on_event, nullptr));
    imu.begin();

    // A full desired table would fail. One sensor cannot fill it.
    if (!imu.enable_sensor(SensorId::ROTATION_VECTOR,
                           {.report_interval_us = ROTATION_VECTOR_INTERVAL_US})
             .ok()) {
        PICO_LOG_ERROR << "could not record the rotation vector";
        return 1;
    }

    // Non-blocking startup otherwise leaves an unresponsive hub silent.
    // startup_phase() identifies the expired deadline (TR-1).
    bool fault_reported = false;
    while (true) {
        imu.service();
        if (imu.faulted() && !fault_reported) {
            fault_reported = true;
            PICO_LOG_ERROR << "bno085 startup faulted in phase "
                           << static_cast<int>(imu.startup_phase())
                           << ", error " << static_cast<int>(imu.fault());
        }
        sleep_ms(1);
    }
}
