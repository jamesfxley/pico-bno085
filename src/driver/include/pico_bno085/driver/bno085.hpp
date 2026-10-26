// SPDX-License-Identifier: MIT
#pragma once

/// \file bno085.hpp
/// User-facing BNO085 driver façade. It owns a pico_sh2::Session over a
/// supplied IHalTransport (built separately for I2C or SPI) and adds the device
/// policy the protocol layer deliberately leaves to the consumer: a
/// desired-sensor table that survives a hub reset (AR-3 re-enable), an
/// enabled-sensor cap (BR-1), the calibration/DCD command forwarders, a
/// normative Stats set (OR-1), and a bridge from the session's diagnostics hook
/// to pico_logging.
///
/// Host-testable: the façade is platform-independent and is driven against
/// pico_sh2::testing::FakeHub in the unit tests; only the transports it is
/// handed are firmware-specific. Every IHalTransport call happens inside
/// begin() or service() (never from an interrupt), matching the session's
/// contract.

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

/// Failures this driver reports (AR-4).
///
/// Its own rather than pico-sh2's. The protocol library's taxonomy is right for
/// the protocol layer, but re-exporting it puts SHTP framing enumerators in
/// front of a caller who has no separate response to them, and lets an append
/// there change this driver's contract without this repository seeing it. The
/// codes below are the distinctions a driver caller can actually act on;
/// from_sh2() collapses the rest.
enum class Error : std::uint8_t {
    /// Success. Zero, which is what Result::ok() compares against.
    NONE = 0,
    /// The underlying I2C or SPI transaction failed.
    BUS_ERROR,
    /// The hub did not answer within the deadline.
    TIMEOUT,
    /// The session is not READY for it: called before the handshake completed,
    /// or cut off by a hub reset that restarted the handshake.
    NOT_READY,
    /// The hub or the protocol layer broke SH-2 or SHTP: a malformed header, a
    /// cargo that will not frame, a dropped reassembly, or a response out of
    /// sequence. One code, because the answer to all four is the same -- reset
    /// the hub and start the handshake again.
    PROTOCOL,
    /// A command or FRS operation is already in flight.
    BUSY,
    /// Nothing has room for the request: the TX queue is full, or a
    /// fixed-capacity table is. Recoverable by retrying once the queue drains
    /// (BR-2).
    NO_ROOM,
    /// Not supported in this build or profile, including a new sensor that
    /// would take the enabled set past the cap (BR-1).
    UNSUPPORTED,
    /// The caller passed an out-of-range or malformed argument.
    INVALID_ARGUMENT,
};

/// This driver's Result: a value or an Error. No exceptions and no heap, and
/// [[nodiscard]] at class scope, so a dropped failure is a compile error.
template <typename T>
using Result = pico_util::Result<T, Error>;

/// Map a pico-sh2 error onto this driver's taxonomy.
///
/// \param error the protocol layer's code.
/// \return the driver code a caller can act on.
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
    // Unreachable from a correctly built tree, and deliberately not a default:
    // label. The switch names every enumerator and carries no default, so a
    // code appended by a later pico-sh2 is a -Wswitch error under -Werror --
    // this repository is told, at build time, that a dependency changed its
    // taxonomy. That is the guarantee (AR-4); this return only keeps the
    // function well-formed for a value outside the enumeration, which a fixed
    // underlying type permits to exist and nothing here produces.
    return Error::PROTOCOL;
}

/// Map a valueless pico-sh2 Result onto this driver's.
///
/// \param result the protocol layer's result.
/// \return the same outcome in this driver's taxonomy.
constexpr Result<void> from_sh2(const pico_sh2::Result<void>& result) {
    if (result.ok()) {
        return {};
    }
    return from_sh2(result.error());
}

/// \name The BNO085's own startup characteristics
/// pico-sh2 carries none of these: it is a protocol library, and how long a
/// part is held in reset, how long it takes to boot, and how quickly it answers
/// are properties of the silicon, not of SH-2 or SHTP. This driver is the
/// BNO085's, so they live here.
/// \{

/// NRST need only be pulsed (1000-3927 §6.5.3, tnrst); 2 ms is a comfortable
/// floor for a host that cannot time a shorter pulse precisely.
inline constexpr std::uint64_t BNO085_RESET_HOLD_US = 2'000;
/// t1 is a 90 ms minimum, with no maximum (1000-3927 §6.5.3).
/// The boot delay and phase deadlines are wedge guards, not derived bounds.
inline constexpr std::uint64_t BNO085_BOOT_DELAY_US = 100'000;
/// A generous multiple of the boot delay, so a slow or busy hub is not failed
/// for still coming up.
inline constexpr std::uint64_t BNO085_STARTUP_PHASE_TIMEOUT_US = 200'000;
/// A command or FRS operation unanswered in half a second is treated as lost
/// rather than waited on.
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

/// The BNO085 advertises SH-2's channels at base 1 — device 1, control 2,
/// inputNormal 3, inputWake 4, inputGyroRv 5 — observed on the part itself.
/// Seeding them lets a reset be requested before the hub has ever advertised,
/// which is the way out of a session stuck short of READY (SR-7). The
/// advertisement overrides the seed, so a part that moved its base is still
/// followed.
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

/// Concurrently enabled sensors the driver tracks, from the active sizing
/// profile. The desired-sensor table and the session's granted-config storage
/// are sized from the same knob so the driver can never desire more than the
/// session can hold (BR-1).
inline constexpr std::size_t MAX_ENABLED_SENSORS =
    pico_sh2::ActiveProfile::MAX_ENABLED_SENSORS;

/// The policy counters the driver is solely responsible for (OR-1).
///
/// The protocol and framing counters belong to the session and are read through
/// counters(); the transport's own belong to whichever transport was handed in,
/// which the caller already holds. Copying either set in here would make
/// pico-sh2's counter list part of this driver's API, so adding a counter there
/// would be a breaking change here.
struct Stats {
    /// Hub resets the session reported to the driver (SR-5), including those
    /// this driver asked for through reset() or clear_dcd_and_reset().
    ///
    /// The session reports a reset from the re-advertisement, which carries no
    /// mark of what caused it. Correlating one with a request the driver made
    /// would mean a spontaneous reset arriving between the request and the
    /// flush being counted as the requested one and so not counted at all, and
    /// undercounting a real fault is worse than counting a deliberate reset.
    std::uint32_t hub_resets = 0;
    /// Times the driver re-reached READY after a reset and re-applied its
    /// desired sensors. Never exceeds \ref hub_resets.
    std::uint32_t recoveries = 0;
    /// Set Feature reports re-issued during those recoveries (cumulative across
    /// recoveries; the initial post-startup apply is not counted here).
    std::uint32_t sensors_reenabled = 0;
    /// enable_sensor() calls refused because the desired table was full (BR-1).
    std::uint32_t config_rejections = 0;
    /// Sensors currently in the desired table.
    std::uint8_t enabled_sensors = 0;
    /// Diagnostic events the session reported (OR-2). Logging them is rate
    /// limited; the count is not.
    std::uint32_t diag_events = 0;
};

/// The BNO085 driver: a pico_sh2::Session plus this part's device policy.
class Bno085 {
   public:
    /// \param hal the transport to drive. It must outlive this object.
    explicit Bno085(pico_sh2::IHalTransport& hal);

    /// \name Not copyable or movable
    /// The session's sinks are bound to this object, so it cannot be copied or
    /// moved without leaving them pointing at the original.
    /// \{
    Bno085(const Bno085&) = delete;
    Bno085& operator=(const Bno085&) = delete;
    Bno085(Bno085&&) = delete;
    Bno085& operator=(Bno085&&) = delete;
    /// \}

    ~Bno085() = default;

    // --- lifecycle -----------------------------------------------------------

    /// Kick the non-blocking startup sequence (reset -> advertisement ->
    /// reset-complete -> initialize). Returns once NRST is asserted; drive
    /// service() to reach ready(). Each startup phase is deadline-bounded by
    /// the session and surfaces a distinct fault (TR-1).
    void begin();

    /// Pump the session one iteration: called every main-loop pass (after a
    /// HINTN wake, or polled). On the transition to READY — initial startup or
    /// recovery after a spontaneous hub reset — the driver re-applies every
    /// desired sensor (AR-3).
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

    /// How far startup got. While faulted() this is the phase the fault
    /// happened in.
    [[nodiscard]] pico_sh2::StartupPhase startup_phase() const {
        return session_.startup_phase();
    }

    // --- sensor surface ------------------------------------------------------

    /// Set where decoded sensor events are delivered. Forwarded to the session.
    void set_event_sink(pico_sh2::reports::EventSink sink) {
        session_.set_event_sink(sink);
    }

    /// How a sensor should run, in the terms the feature report defines
    /// (1000-3625 §6.5.1). A raw flags byte is the wire form; this is what a
    /// caller means.
    struct SensorOptions {
        /// Nominal period between reports (µs). Zero disables the sensor.
        std::uint32_t report_interval_us = 0;
        /// Batch window (µs). Zero delivers each report as it is produced;
        /// non-zero lets the hub group them, and flush() forces an early
        /// delivery.
        std::uint32_t batch_interval_us = 0;
        /// Deliver on the wake channel, which is what a host in a low-power
        /// state watches (feature-flag bit 2).
        bool wake = false;
        /// Keep the sensor running while the hub is in "sleep" (bit 3).
        bool always_on = false;
        /// Report only when the value moves by more than this much, in the
        /// sensor's own units and Q point.
        std::uint16_t change_sensitivity = 0;
        /// Whether \ref change_sensitivity applies (feature-flag bit 1).
        bool change_sensitivity_enabled = false;
        /// Whether it is a fraction of the value rather than an absolute step
        /// (bit 0).
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
        /// Recorded and will be sent by a later service(): either the session
        /// is
        /// not READY yet, or the TX queue was full. Both are ordinary, so they
        /// are not errors — but a caller waiting to see events needs to know
        /// the
        /// hub has not been told yet.
        DEFERRED,
    };

    /// Enable or reconfigure a sensor. The desired configuration is recorded so
    /// it is re-applied after a hub reset, and — when the session is already
    /// READY and the TX queue has room — sent immediately.
    ///
    /// \param sensor the sensor to configure.
    /// \param options how it should run; a zero report_interval_us disables.
    /// \return UNSUPPORTED if a new sensor would exceed the profile's
    ///         enabled-sensor cap (BR-1), without evicting any active sensor.
    Result<EnableState> enable_sensor(pico_sh2::reports::SensorId sensor,
                                      const SensorOptions& options);

    /// Enable or reconfigure a sensor from a full feature configuration.
    ///
    /// \param config the feature configuration to record and send.
    Result<EnableState> enable_sensor(const pico_sh2::FeatureConfig& config);

    /// Disable a sensor: when READY, send a zero-interval Set Feature and, once
    /// it is accepted, drop the sensor from the desired table.
    ///
    /// \param sensor the sensor to disable.
    /// \return ok, including when the sensor was not enabled. NO_ROOM keeps
    ///         the entry so the caller can retry, rather than leaving the
    ///         sensor streaming with no record to disable it.
    Result<void> disable_sensor(pico_sh2::reports::SensorId sensor);

    /// The current desired configuration for a sensor.
    ///
    /// \param sensor the sensor to look up.
    /// \return the configuration, or nullptr when the sensor is not enabled.
    [[nodiscard]] const pico_sh2::FeatureConfig* desired_config(
        pico_sh2::reports::SensorId sensor) const;

    /// Sensors currently in the desired table.
    [[nodiscard]] std::size_t enabled_count() const { return desired_.size(); }

    /// Set where Flush Completed reports are delivered (1000-3625 §6.5.7).
    void set_flush_sink(pico_sh2::Session::FlushSink sink) {
        session_.set_flush_sink(sink);
    }

    /// Ask the hub to send a sensor's outstanding batched data now rather than
    /// waiting for its batch interval (1000-3625 §6.5.6). The hub may flush
    /// other sensors too; each one produces a Flush Completed at the flush
    /// sink.
    ///
    /// \param sensor the sensor to flush.
    Result<void> flush(pico_sh2::reports::SensorId sensor) {
        return from_sh2(session_.force_flush(sensor));
    }

    // --- calibration / DCD forwarders ---------------------------------------

    /// Which axes a tare rotates about (1000-3625 §6.4.3.1, P1).
    ///
    /// The manual documents two bitmaps as supported rather than the whole
    /// eight, so these are the values rather than a bitmap a caller assembles:
    /// an unsupported combination is not a case the hub answers.
    enum class TareAxes : std::uint8_t {
        /// X, Y and Z. Reorients every motion output.
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

    /// What a tare should do. Two enumerations rather than two adjacent
    /// integers, so neither field can be passed where the other belongs.
    struct TareOptions {
        /// Axes to rotate about.
        TareAxes axes = TareAxes::ALL;
        /// Rotation vector the tare is taken against.
        TareBasis basis = TareBasis::ROTATION_VECTOR;
    };

    /// Which motion-engine calibration routines the hub should run (1000-3625
    /// §6.4.6.1, P0-P2). Named fields rather than positional flags: the three
    /// are otherwise interchangeable at a call site and a swap produces a
    /// well-formed command the hub accepts.
    struct CalibrationOptions {
        /// Accelerometer calibration (P0).
        bool accel = false;
        /// Gyroscope calibration (P1).
        bool gyro = false;
        /// Magnetometer calibration (P2).
        bool mag = false;
    };

    /// \name Calibration and DCD forwarders
    /// Thin wrappers over the SH-2 command channel (0xF2). Each returns
    /// NOT_READY before the session is READY, BUSY if a command is already
    /// outstanding, and NO_ROOM if the TX queue is full. The command response
    /// is delivered to the command sink.
    /// \{

    /// Set where command responses are delivered.
    void set_command_sink(pico_sh2::Session::CommandSink sink) {
        session_.set_command_sink(sink);
    }

    /// Tare the reference frame now (Tare, subcommand 0; SH-2 Reference Manual
    /// 1000-3625 §6.4.3.1).
    ///
    /// \param options which axes to tare and against which rotation vector.
    Result<void> tare_now(const TareOptions& options);

    /// Persist the current tare to FRS (Tare, subcommand 1).
    Result<void> persist_tare();

    /// Save the dynamic calibration data to RAM now (Save DCD, 1000-3625
    /// §6.4.5).
    Result<void> save_dcd();

    /// Configure the motion-engine dynamic calibration enables (ME Calibration,
    /// configure subcommand; 1000-3625 §6.4.6.1).
    ///
    /// \param options which calibration routines the hub should run.
    Result<void> configure_dynamic_calibration(
        const CalibrationOptions& options);

    /// Enable or disable periodic DCD save (1000-3625 §6.4.7.1). This command
    /// has no response.
    ///
    /// \param enable whether the hub should save DCD periodically.
    /// P0 is 0x00 to enable and 0x01 to disable (1000-3625 §6.4.7.1).
    Result<void> set_periodic_dcd(bool enable);

    /// Clear the on-chip DCD and reset the hub (1000-3625 §6.4.9.1). The reset
    /// restarts the startup handshake; the driver re-applies desired sensors on
    /// recovery.
    ///
    /// This discards the dynamic calibration the hub has accumulated. To
    /// restart the hub, use reset(); calibration survival is UNCONFIRMED.
    Result<void> clear_dcd_and_reset();

    /// Restart the hub using the executable-channel reset (1000-3600 §2.2). The
    /// handshake runs again and desired sensors are re-applied on recovery.
    Result<void> reset() { return from_sh2(session_.request_reset()); }

    /// Ask which oscillator the hub's clock system uses (1000-3625 §6.4.8). The
    /// type arrives at the command sink in R0.
    Result<void> request_oscillator_type();

    /// Ask the hub to report queued errors (1000-3625 §6.4.1). Each error
    /// arrives at the command sink as its own response; a response whose error
    /// source is 255 ends the sequence.
    ///
    /// \param severity report errors of this severity and higher; 0 is the
    ///        highest.
    Result<void> report_errors(std::uint8_t severity = 0);

    /// Escape hatch for commands without a typed forwarder (1000-3625 §6.4).
    ///
    /// \param command the command number; must be in 1-127.
    /// \param params the command parameters.
    Result<void> send_command(std::uint8_t command,
                              std::span<const std::uint8_t> params = {});

    /// \}

    // --- diagnostics ---------------------------------------------------------

    /// The policy counters this driver owns (OR-1).
    [[nodiscard]] Stats stats() const;

    /// The session's protocol and framing counters, which pico-sh2's
    /// observability requirement specifies and this driver passes through.
    /// Cited by description rather than by ID: OR is this repository's own
    /// prefix, so the ID would resolve to the wrong document (EN-02.8).
    [[nodiscard]] const pico_sh2::SessionCounters& counters() const {
        return session_.counters();
    }

    /// Read-only access to the session, for state the façade does not forward.
    [[nodiscard]] const pico_sh2::Session& session() const { return session_; }

   private:
    // Locate a desired entry by sensor; nullptr if absent.
    pico_sh2::FeatureConfig* find_desired(pico_sh2::reports::SensorId sensor);

    // Record `config` in the desired table (insert or update). Returns the
    // slot, or nullptr if a new entry would exceed the cap (BR-1).
    pico_sh2::FeatureConfig* store_desired(
        const pico_sh2::FeatureConfig& config);

    // Drop a desired entry by sensor; no-op if absent.
    void erase_desired(pico_sh2::reports::SensorId sensor);

    // Enqueue the next batch of desired sensors to the session, resuming from
    // the re-apply cursor. Enqueues stop at the first full TX queue (BR-2); the
    // next service() flushes the queue and resumes, so re-applying more sensors
    // than the queue depth spreads across service() calls.
    void pump_reapply();

    // Schedule a (re)apply of every desired sensor from the start on subsequent
    // service() calls. Set Feature is idempotent, so re-sending an already-
    // applied sensor is harmless. Used when an immediate send is refused by a
    // full TX queue so the recorded desire still reaches the hub.
    void schedule_reapply();

    // Session callbacks (installed in the constructor).
    void on_hub_reset();
    void on_diag(pico_sh2::DiagEvent event);

    pico_sh2::Session session_;

    /// One desired sensor: the configuration, and whether it still has to be
    /// sent in the current re-apply pass.
    ///
    /// The flag replaces an index cursor over the table. A cursor was only
    /// correct while nothing was erased mid-pass, and disable_sensor() can be
    /// called between service() calls -- with dense storage that moved an
    /// unvisited entry behind the cursor and it was never re-sent. Per-entry,
    /// the question is asked of the entry rather than of its position.
    struct Desired {
        pico_sh2::FeatureConfig config{};
        bool pending = false;
    };

    /// The desired-sensor table. Named so pump_reapply() can spell its Entry
    /// and Visit types without repeating the instantiation.
    using DesiredMap = pico_util::FlatMap<pico_sh2::reports::SensorId, Desired,
                                          MAX_ENABLED_SENSORS>;

    DesiredMap desired_;

    bool was_ready_ = false;      // last observed session READY state
    bool pending_reset_ = false;  // a hub reset occurred; recovery is in flight

    // Re-apply state machine: on the rising edge to READY every desired sensor
    // is (re)sent, resumed across service() calls when the TX queue backs up.
    bool reapply_active_ = false;
    bool reapply_after_reset_ = false;  // this re-apply follows a hub reset

    // Diagnostic events seen, and whether each kind has already been logged —
    // a storm must not put printf in the service loop once per event.
    std::uint32_t diag_events_ = 0;
    std::uint32_t diag_logged_kinds_ = 0;

    std::uint32_t hub_resets_ = 0;
    std::uint32_t recoveries_ = 0;
    std::uint32_t sensors_reenabled_ = 0;
    std::uint32_t config_rejections_ = 0;
};

}  // namespace pico_bno085
