// SPDX-License-Identifier: MIT
#pragma once

/// \file bno085.hpp
/// Device policy over pico_sh2::Session: reset recovery, sensor capacity and
/// counters (AR-3, BR-1, OR-1). Transport calls run in begin() or service().

#include <cstddef>
#include <cstdint>
#include <span>

#include "pico_sh2/reports/decoders.hpp"
#include "pico_sh2/reports/sensor_id.hpp"
#include "pico_sh2/sh2/feature.hpp"
#include "pico_sh2/sh2/hal_transport.hpp"
#include "pico_sh2/sh2/profile.hpp"
#include "pico_sh2/sh2/session.hpp"
#include "pico_sh2/sh2/session_timing.hpp"
#include "pico_sh2/sh2/startup_phase.hpp"
#include "pico_sh2/shtp/channels.hpp"
#include "pico_sh2/util/result.hpp"
#include "pico_util/containers/flat_map.hpp"
#include "pico_util/result/result.hpp"

namespace pico_bno085 {

/// Driver errors, mapped from pico-sh2 by from_sh2() (AR-4).
enum class Error : std::uint8_t {
    /// Success. Zero, which is what Result::ok() compares against.
    NONE = 0,
    /// The underlying I2C or SPI transaction failed.
    BUS_ERROR,
    /// The hub did not answer within the deadline.
    TIMEOUT,
    /// The handshake is incomplete or has restarted after a hub reset.
    NOT_READY,
    /// Malformed framing, dropped reassembly or an out-of-sequence response.
    /// Recovery uses a hub reset and a new handshake.
    PROTOCOL,
    /// A command or FRS operation is already in flight.
    BUSY,
    /// The TX queue or a fixed-capacity table is full.
    /// A full queue can be retried after it drains (BR-2).
    NO_ROOM,
    /// The build or profile does not support the request, including a new
    /// sensor beyond the enabled-sensor cap (BR-1).
    UNSUPPORTED,
    /// The caller passed an out-of-range or malformed argument.
    INVALID_ARGUMENT,
};

/// A value or driver error, without exceptions or heap allocation.
/// Discarding a result fails compilation under the required warning flags.
template <typename T>
using Result = pico_util::Result<T, Error>;

/// Map a protocol error to a driver error.
constexpr Error from_sh2(pico_sh2::Sh2Error error) {
    switch (error) {
        case pico_sh2::Sh2Error::NONE:
            return Error::NONE;
        case pico_sh2::Sh2Error::BUS_ERROR:
            return Error::BUS_ERROR;
        case pico_sh2::Sh2Error::TIMEOUT:
            return Error::TIMEOUT;
        case pico_sh2::Sh2Error::NOT_READY:
        case pico_sh2::Sh2Error::HUB_RESET:
            return Error::NOT_READY;
        case pico_sh2::Sh2Error::BAD_HEADER:
        case pico_sh2::Sh2Error::CARGO_TOO_LARGE:
        case pico_sh2::Sh2Error::REASSEMBLY_DROP:
        case pico_sh2::Sh2Error::PROTOCOL:
            return Error::PROTOCOL;
        case pico_sh2::Sh2Error::BUSY:
            return Error::BUSY;
        case pico_sh2::Sh2Error::QUEUE_FULL:
        case pico_sh2::Sh2Error::TABLE_FULL:
            return Error::NO_ROOM;
        case pico_sh2::Sh2Error::UNSUPPORTED:
            return Error::UNSUPPORTED;
        case pico_sh2::Sh2Error::INVALID_ARGUMENT:
            return Error::INVALID_ARGUMENT;
    }
    // No default: a new pico-sh2 enumerator fails -Wswitch under -Werror
    // (AR-4). The return handles values outside the enumeration.
    return Error::PROTOCOL;
}

/// Map a valueless protocol result to a driver result.
constexpr Result<void> from_sh2(const pico_sh2::Result<void>& result) {
    if (result.ok()) {
        return {};
    }
    return from_sh2(result.error());
}

/// \name Startup timing
/// \{

/// Reset hold in microseconds (1000-3927 §6.5.3, tnrst).
inline constexpr std::uint64_t BNO085_RESET_HOLD_US = 2'000;
/// Boot wait in microseconds. This is a timeout guard, not a datasheet maximum
/// (1000-3927 §6.5.3).
inline constexpr std::uint64_t BNO085_BOOT_DELAY_US = 100'000;
/// Startup phase timeout in microseconds.
inline constexpr std::uint64_t BNO085_STARTUP_PHASE_TIMEOUT_US = 200'000;
/// Command and FRS response timeout in microseconds.
inline constexpr std::uint64_t BNO085_TRANSACTION_TIMEOUT_US = 500'000;

/// \}

/// The session timing this part needs, from the constants above.
constexpr pico_sh2::SessionTiming bno085_timing() {
    return {
        .reset_hold_us = BNO085_RESET_HOLD_US,
        .boot_delay_us = BNO085_BOOT_DELAY_US,
        .advertisement_timeout_us = BNO085_STARTUP_PHASE_TIMEOUT_US,
        .reset_complete_timeout_us = BNO085_STARTUP_PHASE_TIMEOUT_US,
        .initialize_timeout_us = BNO085_STARTUP_PHASE_TIMEOUT_US,
        .command_timeout_us = BNO085_TRANSACTION_TIMEOUT_US,
        .frs_timeout_us = BNO085_TRANSACTION_TIMEOUT_US,
    };
}

/// Seeded SH-2 channel base (1000-3927 §1.3.1) (SR-7).
inline constexpr std::uint8_t BNO085_CHANNEL_BASE = 1;

/// The seeded channel map for this part.
constexpr pico_sh2::shtp::ChannelMap bno085_channels() {
    pico_sh2::shtp::ChannelMap map;
    map.seed(pico_sh2::shtp::ChannelRole::DEVICE, BNO085_CHANNEL_BASE);
    map.seed(pico_sh2::shtp::ChannelRole::CONTROL, BNO085_CHANNEL_BASE + 1);
    map.seed(pico_sh2::shtp::ChannelRole::INPUT_NORMAL,
             BNO085_CHANNEL_BASE + 2);
    map.seed(pico_sh2::shtp::ChannelRole::INPUT_WAKE, BNO085_CHANNEL_BASE + 3);
    map.seed(pico_sh2::shtp::ChannelRole::INPUT_GYRO_RV,
             BNO085_CHANNEL_BASE + 4);
    return map;
}

/// Enabled-sensor capacity from the session's active profile (BR-1).
inline constexpr std::size_t MAX_ENABLED_SENSORS =
    pico_sh2::ActiveProfile::MAX_ENABLED_SENSORS;

/// Counters owned by the driver (OR-1).
struct Stats {
    /// Reported hub resets, including reset() and clear_dcd_and_reset()
    /// requests (SR-5).
    std::uint32_t hub_resets = 0;
    /// Returns to READY after a reset. Never exceeds \ref hub_resets.
    std::uint32_t recoveries = 0;
    /// Set Feature reports reissued during recovery, excluding initial startup.
    std::uint32_t sensors_reenabled = 0;
    /// enable_sensor() calls rejected because the desired table was full
    /// (BR-1).
    std::uint32_t config_rejections = 0;
    /// Sensors currently in the desired table.
    std::uint8_t enabled_sensors = 0;
    /// All session diagnostic events, including those not logged (OR-2).
    std::uint32_t diag_events = 0;
};

/// The BNO085 driver: a pico_sh2::Session plus this part's device policy.
class Bno085 {
   public:
    /// \param hal the transport to drive. It must outlive this object.
    explicit Bno085(pico_sh2::IHalTransport& hal);

    /// \name Copy and move
    /// The session sinks retain this object's address.
    /// \{
    Bno085(const Bno085&) = delete;
    Bno085& operator=(const Bno085&) = delete;
    Bno085(Bno085&&) = delete;
    Bno085& operator=(Bno085&&) = delete;
    /// \}

    ~Bno085() = default;

    /// Start without blocking and return with NRST asserted.
    /// Each startup phase has a distinct deadline fault (TR-1).
    void begin();

    /// Advance the session on each main-loop pass.
    /// Desired sensors are reapplied after startup and reset recovery (AR-3).
    void service();

    /// Whether startup has completed and the hub is ready for Set Feature.
    [[nodiscard]] bool ready() const { return session_.ready(); }

    /// The session's current state.
    [[nodiscard]] pico_sh2::SessionState state() const {
        return session_.state();
    }

    /// Whether the session has faulted.
    [[nodiscard]] bool faulted() const { return session_.faulted(); }

    /// The fault, when faulted().
    [[nodiscard]] Error fault() const { return from_sh2(session_.fault()); }

    /// The current startup phase, or the phase where the session faulted.
    [[nodiscard]] pico_sh2::StartupPhase startup_phase() const {
        return session_.startup_phase();
    }

    /// Set where decoded sensor events are delivered. Forwarded to the session.
    void set_event_sink(pico_sh2::reports::EventSink sink) {
        session_.set_event_sink(sink);
    }

    /// How a sensor should run (1000-3625 §6.5.1).
    struct SensorOptions {
        /// Nominal period between reports (µs). Zero disables the sensor.
        std::uint32_t report_interval_us = 0;
        /// Batch window in microseconds. Zero sends reports as produced; a
        /// non-zero window is the maximum delivery delay, and flush() delivers
        /// early.
        std::uint32_t batch_interval_us = 0;
        /// Deliver on the low-power host's wake channel (feature-flag bit 2).
        bool wake = false;
        /// Keep the sensor running while the hub is in "sleep" (bit 3).
        bool always_on = false;
        /// Change threshold in the sensor's units and Q point.
        std::uint16_t change_sensitivity = 0;
        /// Whether \ref change_sensitivity applies (feature-flag bit 1).
        bool change_sensitivity_enabled = false;
        /// Whether change_sensitivity is relative to the value (bit 0).
        bool change_sensitivity_relative = false;

        /// The wire flags byte these options mean.
        [[nodiscard]] std::uint8_t flags() const {
            std::uint8_t out = 0;
            if (change_sensitivity_relative) {
                out |= pico_sh2::feature_flag::CHANGE_SENSITIVITY_RELATIVE;
            }
            if (change_sensitivity_enabled) {
                out |= pico_sh2::feature_flag::CHANGE_SENSITIVITY_ENABLED;
            }
            if (wake) {
                out |= pico_sh2::feature_flag::WAKE;
            }
            if (always_on) {
                out |= pico_sh2::feature_flag::ALWAYS_ON;
            }
            return out;
        }
    };

    /// Whether a recorded sensor configuration has reached the hub yet.
    enum class EnableState : std::uint8_t {
        /// Sent to the hub on this call.
        APPLIED,
        /// Recorded for a later service() because the session is not READY or
        /// the TX queue is full.
        DEFERRED,
    };

    /// Record a sensor configuration for immediate send or later reapplication.
    /// \param sensor the sensor to configure.
    /// \param options how it should run; zero report_interval_us disables.
    /// \return UNSUPPORTED for a new sensor beyond the cap. No sensor is
    /// evicted (BR-1).
    Result<EnableState> enable_sensor(pico_sh2::reports::SensorId sensor,
                                      const SensorOptions& options);

    /// Record and send a complete feature configuration.
    Result<EnableState> enable_sensor(const pico_sh2::FeatureConfig& config);

    /// Disable a sensor and remove its desired entry after acceptance when
    /// READY.
    /// \return Success, including when the sensor is absent.
    /// NO_ROOM keeps the entry for a retry.
    Result<void> disable_sensor(pico_sh2::reports::SensorId sensor);

    /// Look up a desired configuration.
    /// \return nullptr if the sensor is not enabled.
    [[nodiscard]] const pico_sh2::FeatureConfig* desired_config(
        pico_sh2::reports::SensorId sensor) const;

    /// Sensors currently in the desired table.
    [[nodiscard]] std::size_t enabled_count() const { return desired_.size(); }

    /// Set where Flush Completed reports are delivered (1000-3625 §6.5.7).
    void set_flush_sink(pico_sh2::Session::FlushSink sink) {
        session_.set_flush_sink(sink);
    }

    /// Request outstanding batched data now (1000-3625 §6.5.6).
    /// The hub can flush other sensors too. Each reports completion to the
    /// flush sink.
    Result<void> flush(pico_sh2::reports::SensorId sensor) {
        return from_sh2(session_.force_flush(sensor));
    }

    /// Supported tare axes (1000-3625 §6.4.3.1, P1).
    enum class TareAxes : std::uint8_t {
        /// Rotation about X, Y and Z. Affected outputs depend on TareBasis.
        ALL = 0x07,
        /// Z alone: changes heading, leaves tilt.
        Z_ONLY = 0x04,
    };

    /// The rotation vector a tare is taken against (1000-3625 §6.4.3.1, P2).
    enum class TareBasis : std::uint8_t {
        ROTATION_VECTOR = 0,
        GAMING_ROTATION_VECTOR = 1,
        GEOMAGNETIC_ROTATION_VECTOR = 2,
        GYRO_INTEGRATED_ROTATION_VECTOR = 3,
        ARVR_STABILIZED_ROTATION_VECTOR = 4,
        ARVR_STABILIZED_GAME_ROTATION_VECTOR = 5,
    };

    /// What a tare should do.
    struct TareOptions {
        /// Axes to rotate about.
        TareAxes axes = TareAxes::ALL;
        /// Rotation vector the tare is taken against.
        TareBasis basis = TareBasis::ROTATION_VECTOR;
    };

    /// Which calibration routines the hub should run (1000-3625 §6.4.6.1,
    /// P0-P2).
    struct CalibrationOptions {
        /// Accelerometer calibration (P0).
        bool accel = false;
        /// Gyroscope calibration (P1).
        bool gyro = false;
        /// Magnetometer calibration (P2).
        bool mag = false;
    };

    /// \name Calibration and DCD commands
    /// Commands return NOT_READY before READY, BUSY while one is outstanding,
    /// and NO_ROOM when the TX queue is full. Responses go to the command sink.
    /// \{

    /// Set where command responses are delivered.
    void set_command_sink(pico_sh2::Session::CommandSink sink) {
        session_.set_command_sink(sink);
    }

    /// Tare the reference frame now (1000-3625 §6.4.3.1, subcommand 0).
    Result<void> tare_now(const TareOptions& options);

    /// Persist the current tare to FRS (Tare, subcommand 1).
    Result<void> persist_tare();

    /// Save dynamic calibration data to FRS (1000-3625 §6.4.5; 1000-3927 §3.4).
    Result<void> save_dcd();

    /// Configure dynamic calibration (1000-3625 §6.4.6.1).
    /// \param options which calibration routines the hub should run.
    Result<void> configure_dynamic_calibration(
        const CalibrationOptions& options);

    /// Enable or disable periodic DCD save without a response (1000-3625
    /// §6.4.7.1).
    /// \param enable whether the hub should save DCD periodically. P0 is 0 to
    /// enable, 1 to disable.
    Result<void> set_periodic_dcd(bool enable);

    /// Discard dynamic calibration and reset. Desired sensors are reapplied
    /// after the handshake (1000-3625 §6.4.9.1).
    /// Use reset() to restart without clearing DCD. Calibration survival is
    /// UNCONFIRMED (FW-2).
    Result<void> clear_dcd_and_reset();

    /// Restart through the executable channel and reapply desired sensors
    /// after the handshake (1000-3600 §2.2).
    Result<void> reset() { return from_sh2(session_.request_reset()); }

    /// Request the oscillator type, delivered to the command sink in R0
    /// (1000-3625 §6.4.8).
    Result<void> request_oscillator_type();

    /// Request queued errors, each delivered as a command response (1000-3625
    /// §6.4.1). Error source 255 ends the sequence.
    /// \param severity report errors at this severity or higher; 0 is highest.
    Result<void> report_errors(std::uint8_t severity = 0);

    /// Send a command without a typed wrapper (1000-3625 §6.4).
    /// \param command the command number; must be in 1-127.
    /// \param params command parameter bytes.
    Result<void> send_command(std::uint8_t command,
                              std::span<const std::uint8_t> params = {});

    /// \}

    /// The policy counters this driver owns (OR-1).
    [[nodiscard]] Stats stats() const;

    /// The session's protocol and framing counters, specified by pico-sh2's
    /// observability requirement. It is cited by name because OR is also this
    /// repository's prefix.
    [[nodiscard]] const pico_sh2::SessionCounters& counters() const {
        return session_.counters();
    }

    /// Read-only access to the session, for state the façade does not forward.
    [[nodiscard]] const pico_sh2::Session& session() const { return session_; }

   private:
    pico_sh2::FeatureConfig* find_desired(pico_sh2::reports::SensorId sensor);

    // Insert or update a configuration. Return nullptr for a new sensor
    // beyond the cap (BR-1).
    pico_sh2::FeatureConfig* store_desired(
        const pico_sh2::FeatureConfig& config);

    void erase_desired(pico_sh2::reports::SensorId sensor);

    // Enqueue pending sensors in table order until the TX queue fills.
    // The next service() flushes the queue and resumes (BR-2).
    void pump_reapply();

    // Mark all entries pending for subsequent service() calls.
    void schedule_reapply();

    void on_hub_reset();
    void on_diag(pico_sh2::DiagEvent event);

    pico_sh2::Session session_;

    /// A desired configuration and its pending-send flag.
    struct Desired {
        pico_sh2::FeatureConfig config{};
        bool pending = false;
    };

    /// The desired-sensor table type.
    using DesiredMap = pico_util::FlatMap<pico_sh2::reports::SensorId, Desired,
                                          MAX_ENABLED_SENSORS>;

    DesiredMap desired_;

    bool was_ready_ = false;      // last observed session READY state
    bool pending_reset_ = false;  // a hub reset occurred; recovery is in flight

    // A reapplication pass can span several service() calls.
    bool reapply_active_ = false;
    bool reapply_after_reset_ = false;  // this re-apply follows a hub reset

    // A diagnostic storm must not put printf in service() once per event.
    std::uint32_t diag_events_ = 0;
    std::uint32_t diag_logged_kinds_ = 0;

    std::uint32_t hub_resets_ = 0;
    std::uint32_t recoveries_ = 0;
    std::uint32_t sensors_reenabled_ = 0;
    std::uint32_t config_rejections_ = 0;
};

}  // namespace pico_bno085
