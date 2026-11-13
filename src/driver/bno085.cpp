// SPDX-License-Identifier: MIT
#include "pico_bno085/driver/bno085.hpp"

#include "pico_bno085/pico_bno085.hpp"

// Check the header version in firmware and consumer builds, where the host
// version test does not run.
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
    // A reset can complete its handshake within one service() call, leaving
    // READY unchanged.
    if (now_ready && (!was_ready_ || pending_reset_)) {
        schedule_reapply();
        reapply_after_reset_ = pending_reset_;
        if (pending_reset_) {
            ++recoveries_;
            pending_reset_ = false;
        }
    } else if (!now_ready) {
        reapply_active_ = false;
    }
    was_ready_ = now_ready;
    if (now_ready && reapply_active_) {
        pump_reapply();
    }
}

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
        // A new sensor at the cap evicts no active sensor (BR-1).
        return Error::UNSUPPORTED;
    }
    if (!session_.ready()) {
        return EnableState::DEFERRED;
    }
    const pico_sh2::Result<void> sent = session_.set_feature(*slot);
    if (sent.ok()) {
        return EnableState::APPLIED;
    }
    // QUEUE_FULL and TABLE_FULL both map to NO_ROOM, but only QUEUE_FULL
    // is retryable here.
    if (sent.error() == Sh2Error::QUEUE_FULL) {
        // The recorded configuration reaches the hub after the queue drains
        // (BR-2).
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
            // The desired entry stays available for a retry.
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
    // try_emplace preserves an existing entry's pending flag and returns
    // nullptr for a new sensor at the cap (BR-1).
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
            // The pending entry survives enable/disable calls between service()
            // calls.
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
    // A diagnostic storm must not delay service() enough to miss a deadline.
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
