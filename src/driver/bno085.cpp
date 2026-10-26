// SPDX-License-Identifier: MIT
#include "pico_bno085/driver/bno085.hpp"

#include "pico_bno085/pico_bno085.hpp"

// The advertised version and project(pico_bno085 VERSION ...) are two records
// of one fact. Bumping either alone stops the build here.
//
// The constants live in the umbrella header, whose target is INTERFACE and has
// no translation unit of its own; this is the driver's compiled one. The CMake
// values arrive PRIVATE, so they reach no installed consumer. A host test
// asserts the same pair, but that test is skipped on every configure with tests
// off -- which is every firmware and every consumer build.
static_assert(pico_bno085::VERSION_MAJOR == PICO_BNO085_CMAKE_VERSION_MAJOR);
static_assert(pico_bno085::VERSION_MINOR == PICO_BNO085_CMAKE_VERSION_MINOR);
static_assert(pico_bno085::VERSION_PATCH == PICO_BNO085_CMAKE_VERSION_PATCH);

#include <array>
#include <cstdint>
#include <span>

#include "pico_logging/logging/logging.hpp"
#include "pico_sh2/reports/sensor_id.hpp"
#include "pico_sh2/sh2/command.hpp"
#include "pico_sh2/sh2/feature.hpp"
#include "pico_sh2/sh2/hal_transport.hpp"
#include "pico_sh2/sh2/session.hpp"
#include "pico_sh2/util/result.hpp"

namespace pico_bno085 {

using pico_sh2::FeatureConfig;
using pico_sh2::Sh2Error;
using pico_sh2::reports::SensorId;

Bno085::Bno085(pico_sh2::IHalTransport& hal)
    : session_(hal, bno085_timing(), bno085_channels()) {
    session_.set_reset_sink(
        pico_sh2::Session::ResetSink::bind<Bno085, &Bno085::on_hub_reset>(
            this));
    session_.set_diag_sink(
        pico_sh2::Session::DiagSink::bind<Bno085, &Bno085::on_diag>(this));
}

void Bno085::begin() {
    PICO_LOG_INFO << "bno085: begin";
    was_ready_ = false;
    pending_reset_ = false;
    session_.begin();
}

void Bno085::service() {
    session_.service();
    const bool now_ready = session_.ready();
    // Re-apply on the rising edge to READY, and also whenever a hub reset is
    // still outstanding: a reset whose whole re-handshake is drained inside one
    // service() never shows an edge, and keying only on the edge would leave
    // the hub streaming nothing with the desired table saying otherwise.
    if (now_ready && (!was_ready_ || pending_reset_)) {
        schedule_reapply();
        reapply_after_reset_ = pending_reset_;
        if (pending_reset_) {
            ++recoveries_;
            pending_reset_ = false;
        }
    } else if (!now_ready) {
        // A reset dropped us out of READY; abandon any in-flight re-apply.
        reapply_active_ = false;
    }
    was_ready_ = now_ready;
    if (now_ready && reapply_active_) {
        pump_reapply();
    }
}

// --- sensor surface ---------------------------------------------------------

Result<Bno085::EnableState> Bno085::enable_sensor(
    SensorId sensor, const SensorOptions& options) {
    FeatureConfig config;
    config.sensor = sensor;
    config.flags = options.flags();
    config.change_sensitivity = options.change_sensitivity;
    config.report_interval_us = options.report_interval_us;
    config.batch_interval_us = options.batch_interval_us;
    return enable_sensor(config);
}

Result<Bno085::EnableState> Bno085::enable_sensor(const FeatureConfig& config) {
    FeatureConfig* slot = store_desired(config);
    if (slot == nullptr) {
        ++config_rejections_;
        // BR-1: the cap is reached and this is a new sensor; no active sensor
        // is evicted.
        return Error::UNSUPPORTED;
    }
    if (!session_.ready()) {
        // Recorded; the rising edge to READY applies it.
        return EnableState::DEFERRED;
    }
    const pico_sh2::Result<void> sent = session_.set_feature(*slot);
    if (sent.ok()) {
        return EnableState::APPLIED;
    }
    // Against pico-sh2's own code, not this driver's: QUEUE_FULL and TABLE_FULL
    // both map to NO_ROOM, and only the first of them is retryable here.
    if (sent.error() == Sh2Error::QUEUE_FULL) {
        // The desired entry is already recorded, so a full TX queue is not
        // fatal: schedule a re-apply and the recorded desire reaches the hub on
        // a later service() once the queue drains (BR-2).
        schedule_reapply();
        return EnableState::DEFERRED;
    }
    return from_sh2(sent.error());
}

Result<void> Bno085::disable_sensor(SensorId sensor) {
    FeatureConfig* slot = find_desired(sensor);
    if (slot == nullptr) {
        return Result<void>{};
    }
    if (session_.ready()) {
        FeatureConfig off = *slot;
        off.report_interval_us = 0;
        off.batch_interval_us = 0;
        const pico_sh2::Result<void> sent = session_.set_feature(off);
        if (!sent.ok()) {
            // Keep the desired entry so the caller can retry; dropping it now
            // would leave the sensor streaming with no record to disable it.
            return from_sh2(sent.error());
        }
    }
    erase_desired(sensor);
    return Result<void>{};
}

const FeatureConfig* Bno085::desired_config(SensorId sensor) const {
    const Desired* entry = desired_.find(sensor);
    return entry != nullptr ? &entry->config : nullptr;
}

FeatureConfig* Bno085::find_desired(SensorId sensor) {
    Desired* entry = desired_.find(sensor);
    return entry != nullptr ? &entry->config : nullptr;
}

FeatureConfig* Bno085::store_desired(const FeatureConfig& config) {
    // try_emplace rather than insert_or_assign: it reports the cap being
    // reached by returning nullptr for a new sensor (BR-1) while leaving an
    // existing one findable, and it does not disturb the pending flag of an
    // entry being reconfigured.
    Desired* entry = desired_.try_emplace(config.sensor);
    if (entry == nullptr) {
        return nullptr;
    }
    entry->config = config;
    return &entry->config;
}

void Bno085::erase_desired(SensorId sensor) { desired_.erase(sensor); }

void Bno085::pump_reapply() {
    bool paused = false;
    desired_.for_each([this, &paused](SensorId, Desired& desired) {
        if (!desired.pending) {
            return DesiredMap::Visit::CONTINUE;
        }
        const pico_sh2::Result<void> result =
            session_.set_feature(desired.config);
        if (!result.ok()) {
            // TX queue full: pause. The entry stays pending, so the next
            // service() flushes the queue and resumes with this one -- whatever
            // has been enabled or disabled in between.
            paused = true;
            return DesiredMap::Visit::STOP;
        }
        desired.pending = false;
        if (reapply_after_reset_) {
            ++sensors_reenabled_;
        }
        return DesiredMap::Visit::CONTINUE;
    });
    if (!paused) {
        reapply_active_ = false;
    }
}

void Bno085::schedule_reapply() {
    reapply_active_ = true;
    desired_.for_each([](SensorId, Desired& desired) {
        desired.pending = true;
        return DesiredMap::Visit::CONTINUE;
    });
}

// --- calibration / DCD forwarders -------------------------------------------

Result<void> Bno085::tare_now(const TareOptions& options) {
    // P0 subcommand, P1 axes, P2 basis (1000-3625 §6.4.3.1).
    std::array<std::uint8_t, 3> params{
        0, static_cast<std::uint8_t>(options.axes),
        static_cast<std::uint8_t>(options.basis)};
    return from_sh2(session_.send_command(pico_sh2::command::TARE, params));
}

Result<void> Bno085::persist_tare() {
    std::array<std::uint8_t, 1> params{1};
    return from_sh2(session_.send_command(pico_sh2::command::TARE, params));
}

Result<void> Bno085::save_dcd() {
    return from_sh2(session_.send_command(pico_sh2::command::SAVE_DCD));
}

Result<void> Bno085::configure_dynamic_calibration(
    const CalibrationOptions& options) {
    // P0 accel, P1 gyro, P2 mag, P3 subcommand (1000-3625 §6.4.6.1).
    std::array<std::uint8_t, 4> params{
        static_cast<std::uint8_t>(options.accel ? 1 : 0),
        static_cast<std::uint8_t>(options.gyro ? 1 : 0),
        static_cast<std::uint8_t>(options.mag ? 1 : 0),
        0,  // subcommand 0: configure
    };
    return from_sh2(
        session_.send_command(pico_sh2::command::ME_CALIBRATION, params));
}

Result<void> Bno085::set_periodic_dcd(bool enable) {
    // 1000-3625 6.4.7.1, Figure 59: zero enables; one disables.
    std::array<std::uint8_t, 1> params{
        static_cast<std::uint8_t>(enable ? 0x00 : 0x01)};
    return from_sh2(
        session_.send_command(pico_sh2::command::PERIODIC_DCD, params));
}

Result<void> Bno085::clear_dcd_and_reset() {
    return from_sh2(
        session_.send_command(pico_sh2::command::CLEAR_DCD_AND_RESET));
}

Result<void> Bno085::request_oscillator_type() {
    return from_sh2(session_.send_command(pico_sh2::command::OSCILLATOR_TYPE));
}

Result<void> Bno085::report_errors(std::uint8_t severity) {
    std::array<std::uint8_t, 1> params{severity};
    return from_sh2(
        session_.send_command(pico_sh2::command::REPORT_ERRORS, params));
}

Result<void> Bno085::send_command(std::uint8_t command,
                                  std::span<const std::uint8_t> params) {
    return from_sh2(session_.send_command(command, params));
}

// --- diagnostics ------------------------------------------------------------

Stats Bno085::stats() const {
    Stats stats;
    stats.hub_resets = hub_resets_;
    stats.recoveries = recoveries_;
    stats.sensors_reenabled = sensors_reenabled_;
    stats.config_rejections = config_rejections_;
    stats.enabled_sensors = static_cast<std::uint8_t>(desired_.size());
    stats.diag_events = diag_events_;
    return stats;
}

void Bno085::on_hub_reset() {
    ++hub_resets_;
    pending_reset_ = true;
    PICO_LOG_WARN << "bno085: hub reset; re-enabling sensors on recovery";
}

void Bno085::on_diag(pico_sh2::DiagEvent event) {
    ++diag_events_;
    // This runs inside service(), between a transfer and the next one the hub
    // is waiting to be read, and a misbehaving hub can raise an event per
    // transfer. Log the first of each kind and count the rest: a storm must not
    // put a log line in the service loop often enough to miss a deadline.
    const auto kind = static_cast<std::uint8_t>(event);
    const std::uint32_t bit = 1U << (kind % 32U);
    if ((diag_logged_kinds_ & bit) != 0) {
        return;
    }
    diag_logged_kinds_ |= bit;
    PICO_LOG_WARN << "bno085: diag event " << static_cast<int>(kind)
                  << " (further events of this kind are counted only)";
}

}  // namespace pico_bno085
