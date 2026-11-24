// SPDX-License-Identifier: MIT
#include "pico_bno085/driver/bno085.hpp"

#include <gtest/gtest.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <random>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

#include "pico_sh2/reports/sensor_id.hpp"
#include "pico_sh2/sh2/command.hpp"
#include "pico_sh2/sh2/feature.hpp"
#include "pico_sh2/sh2/hal_transport.hpp"
#include "pico_sh2/sh2/profile.hpp"
#include "pico_sh2/sh2/session.hpp"
#include "pico_sh2/sh2/startup_phase.hpp"
#include "pico_sh2/shtp/channels.hpp"
#include "pico_sh2/testing/fake_hub.hpp"
#include "pico_sh2/util/result.hpp"

namespace pico_bno085 {
namespace {

using pico_sh2::reports::SensorId;
using pico_sh2::testing::FakeHub;

std::vector<std::uint8_t> frame(std::uint8_t channel, std::uint8_t seq,
                                std::vector<std::uint8_t> payload) {
    const std::size_t total = payload.size() + 4;
    std::vector<std::uint8_t> out;
    // reserve lets GCC 13 bound insert under _FORTIFY_SOURCE=3 at -O3,
    // avoiding a stringop-overflow diagnostic.
    out.reserve(total);
    out.push_back(static_cast<std::uint8_t>(total & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((total >> 8U) & 0x7FU));
    out.push_back(channel);
    out.push_back(seq);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// Minimal channel-0 advertisement: response 0, sensorhub GUID 2 and
// one tag-0x81 report-length pair (1000-3535 §5.1.1.1).
std::vector<std::uint8_t> advertisement() {
    return {0x00, 0x01, 0x01, 0x02, 0x81, 0x02, 0x05, 0x18};
}

// The unsolicited Initialize response: 0xF1, command byte 0x84, R0 = status.
std::vector<std::uint8_t> initialize_response(std::uint8_t status) {
    std::vector<std::uint8_t> frame(16, 0);
    frame[0] = 0xF1;
    frame[2] = 0x84;
    frame[5] = status;
    return frame;
}

// Drive startup or recovery to READY using re_seq as the channel sequence.
void drive_to_ready(FakeHub& hub, Bno085& driver, std::uint8_t re_seq = 0) {
    if (re_seq == 0) {
        driver.begin();
        hub.advance_time(2'000);  // reset hold
        driver.service();
        hub.advance_time(100'000);  // boot delay
        driver.service();
    }
    hub.queue_transfer(frame(0, re_seq, advertisement()), 0);
    hub.queue_transfer(frame(1, re_seq, {0x01}), 0);
    hub.queue_transfer(frame(2, re_seq, initialize_response(0)), 0);
    driver.service();
    ASSERT_TRUE(driver.ready());
}

std::uint8_t write_report_id(const FakeHub::CapturedWrite& write) {
    return write.frame.at(4);
}

int count_set_feature(const FakeHub& hub, SensorId sensor) {
    int n = 0;
    for (const auto& write : hub.writes()) {
        if (write_report_id(write) == pico_sh2::SET_FEATURE_COMMAND &&
            write.frame.at(5) == static_cast<std::uint8_t>(sensor)) {
            ++n;
        }
    }
    return n;
}

// Driver errors use the local taxonomy (AR-4).

TEST(Bno085Error, MapsEachProtocolCodeToOneADriverCallerCanActOn) {
    EXPECT_EQ(from_sh2(pico_sh2::Sh2Error::NONE), Error::NONE);
    EXPECT_EQ(from_sh2(pico_sh2::Sh2Error::BUS_ERROR), Error::BUS_ERROR);
    EXPECT_EQ(from_sh2(pico_sh2::Sh2Error::TIMEOUT), Error::TIMEOUT);
    EXPECT_EQ(from_sh2(pico_sh2::Sh2Error::NOT_READY), Error::NOT_READY);
    EXPECT_EQ(from_sh2(pico_sh2::Sh2Error::HUB_RESET), Error::NOT_READY);
    EXPECT_EQ(from_sh2(pico_sh2::Sh2Error::BUSY), Error::BUSY);
    EXPECT_EQ(from_sh2(pico_sh2::Sh2Error::UNSUPPORTED), Error::UNSUPPORTED);
    EXPECT_EQ(from_sh2(pico_sh2::Sh2Error::INVALID_ARGUMENT),
              Error::INVALID_ARGUMENT);
}

TEST(Bno085Error, CollapsesTheFramingCodesADriverCallerCannotSeparate) {
    // All four framing errors use hub reset as recovery.
    EXPECT_EQ(from_sh2(pico_sh2::Sh2Error::BAD_HEADER), Error::PROTOCOL);
    EXPECT_EQ(from_sh2(pico_sh2::Sh2Error::CARGO_TOO_LARGE), Error::PROTOCOL);
    EXPECT_EQ(from_sh2(pico_sh2::Sh2Error::REASSEMBLY_DROP), Error::PROTOCOL);
    EXPECT_EQ(from_sh2(pico_sh2::Sh2Error::PROTOCOL), Error::PROTOCOL);

    // Both capacity codes are "retry once something drains" (BR-2).
    EXPECT_EQ(from_sh2(pico_sh2::Sh2Error::QUEUE_FULL), Error::NO_ROOM);
    EXPECT_EQ(from_sh2(pico_sh2::Sh2Error::TABLE_FULL), Error::NO_ROOM);
}

TEST(Bno085Error, SuccessIsZeroSoResultOkRestsOnIt) {
    // NONE must be value-initialised because Result compares errors with E{}.
    EXPECT_EQ(static_cast<std::uint8_t>(Error{}), 0U);
    EXPECT_EQ(Error{}, Error::NONE);
    EXPECT_TRUE(Result<void>{}.ok());
}

// Startup records the phase of an expired deadline (TR-1).

TEST(Bno085, StartupReachesReady) {
    FakeHub hub;
    Bno085 driver(hub);
    EXPECT_FALSE(driver.ready());
    drive_to_ready(hub, driver);
    EXPECT_EQ(driver.state(), pico_sh2::SessionState::READY);
    EXPECT_FALSE(driver.faulted());
}

TEST(Bno085, StartupDeadlineFaultsWithDistinctPhase) {
    FakeHub hub;
    Bno085 driver(hub);
    driver.begin();
    hub.advance_time(2'000);
    driver.service();
    hub.advance_time(100'000);
    driver.service();  // WAIT_ADVERTISEMENT
    // No advertisement arrives; the phase deadline expires (TR-1).
    hub.advance_time(1'000'000);
    driver.service();
    EXPECT_TRUE(driver.faulted());
    EXPECT_EQ(driver.fault(), Error::TIMEOUT);
    EXPECT_EQ(driver.startup_phase(),
              pico_sh2::StartupPhase::WAIT_ADVERTISEMENT);
}

// The two later phases fault the same way, each recording its phase (TR-1).
TEST(Bno085, LaterStartupDeadlinesFaultWithTheirOwnPhase) {
    for (const bool initialize_phase : {false, true}) {
        FakeHub hub;
        Bno085 driver(hub);
        driver.begin();
        hub.advance_time(2'000);
        driver.service();
        hub.advance_time(100'000);
        driver.service();
        hub.queue_transfer(frame(0, 0, advertisement()), 0);
        if (initialize_phase) {
            hub.queue_transfer(frame(1, 0, {0x01}), 0);
        }
        driver.service();
        hub.advance_time(1'000'000);
        driver.service();
        EXPECT_TRUE(driver.faulted());
        EXPECT_EQ(driver.fault(), Error::TIMEOUT);
        EXPECT_EQ(driver.startup_phase(),
                  initialize_phase
                      ? pico_sh2::StartupPhase::WAIT_INITIALIZE
                      : pico_sh2::StartupPhase::WAIT_RESET_COMPLETE);
    }
}

// A transport reporting no data is never read (HR-1).
// Device observations cannot distinguish a blind read of a present hub from
// silence.
class CountingTransport final : public pico_sh2::IHalTransport {
   public:
    explicit CountingTransport(FakeHub& inner) : inner_(&inner) {}

    [[nodiscard]] int reads() const { return reads_; }

    bool data_ready() override { return inner_->data_ready(); }

    pico_sh2::Result<pico_sh2::ReceivedTransfer> read_transfer(
        std::span<std::uint8_t> dst) override {
        ++reads_;
        return inner_->read_transfer(dst);
    }

    pico_sh2::Result<void> write_transfer(
        std::span<const std::uint8_t> frame) override {
        return inner_->write_transfer(frame);
    }

    std::uint64_t now_us() override { return inner_->now_us(); }

    void set_reset(bool asserted) override { inner_->set_reset(asserted); }

   private:
    FakeHub* inner_;
    int reads_ = 0;
};

TEST(Bno085, ATransportReportingNoDataIsNeverRead) {
    FakeHub hub;
    CountingTransport counting(hub);
    Bno085 driver(counting);
    driver.begin();
    hub.advance_time(2'000);
    driver.service();
    hub.advance_time(100'000);
    driver.service();
    hub.queue_transfer(frame(0, 0, advertisement()), 0);
    hub.queue_transfer(frame(1, 0, {0x01}), 0);
    hub.queue_transfer(frame(2, 0, initialize_response(0)), 0);
    driver.service();
    ASSERT_TRUE(driver.ready());

    const int at_ready = counting.reads();
    for (int i = 0; i < 100; ++i) {
        hub.advance_time(10'000);
        driver.service();
    }
    EXPECT_EQ(counting.reads(), at_ready);

    hub.queue_idle_transfer();
    driver.service();
    EXPECT_EQ(counting.reads(), at_ready + 1);
}

// Desired configurations survive startup and recovery (AR-3).

// Both applied and deferred configurations are successful outcomes.
TEST(Bno085, EnableReportsWhetherTheHubHasBeenTold) {
    FakeHub hub;
    Bno085 driver(hub);

    auto before = driver.enable_sensor(SensorId::ROTATION_VECTOR,
                                       {.report_interval_us = 10'000});
    ASSERT_TRUE(before.ok());
    EXPECT_EQ(before.value(), Bno085::EnableState::DEFERRED);

    drive_to_ready(hub, driver);
    auto after = driver.enable_sensor(SensorId::ACCELEROMETER,
                                      {.report_interval_us = 10'000});
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value(), Bno085::EnableState::APPLIED);
}

// A full queue defers the recorded configuration for reapplication (BR-2).
TEST(Bno085, EnableDefersWhenTheQueueIsFull) {
    FakeHub hub;
    Bno085 driver(hub);
    drive_to_ready(hub, driver);

    Bno085::EnableState last = Bno085::EnableState::APPLIED;
    for (std::size_t i = 0; i < pico_sh2::ActiveProfile::TX_QUEUE_DEPTH + 1;
         ++i) {
        auto result = driver.enable_sensor(
            static_cast<SensorId>(0x01 + static_cast<std::uint8_t>(i)),
            {.report_interval_us = 10'000});
        ASSERT_TRUE(result.ok());
        last = result.value();
    }
    EXPECT_EQ(last, Bno085::EnableState::DEFERRED);

    const auto deferred = static_cast<SensorId>(
        0x01 +
        static_cast<std::uint8_t>(pico_sh2::ActiveProfile::TX_QUEUE_DEPTH));
    for (int i = 0; i < 4; ++i) {
        driver.service();
    }
    EXPECT_EQ(count_set_feature(hub, deferred), 1);
}

TEST(Bno085, SensorEnabledBeforeReadyIsAppliedOnReady) {
    FakeHub hub;
    Bno085 driver(hub);
    ASSERT_TRUE(driver
                    .enable_sensor(SensorId::ROTATION_VECTOR,
                                   {.report_interval_us = 10'000})
                    .ok());
    EXPECT_EQ(count_set_feature(hub, SensorId::ROTATION_VECTOR), 0);

    drive_to_ready(hub, driver);
    // The next service() flushes the configuration enqueued on reaching READY.
    driver.service();
    EXPECT_EQ(count_set_feature(hub, SensorId::ROTATION_VECTOR), 1);
    EXPECT_EQ(driver.enabled_count(), 1U);
}

TEST(Bno085, SensorEnabledWhileReadyIsSentImmediately) {
    FakeHub hub;
    Bno085 driver(hub);
    drive_to_ready(hub, driver);
    ASSERT_TRUE(driver
                    .enable_sensor(SensorId::ACCELEROMETER,
                                   {.report_interval_us = 5'000})
                    .ok());
    driver.service();  // flush the enqueued Set Feature
    EXPECT_EQ(count_set_feature(hub, SensorId::ACCELEROMETER), 1);

    const pico_sh2::FeatureConfig* cfg =
        driver.desired_config(SensorId::ACCELEROMETER);
    ASSERT_NE(cfg, nullptr);
    EXPECT_EQ(cfg->report_interval_us, 5'000U);
}

TEST(Bno085, ReEnablingUpdatesInPlaceWithoutConsumingCap) {
    FakeHub hub;
    Bno085 driver(hub);
    drive_to_ready(hub, driver);
    ASSERT_TRUE(
        driver.enable_sensor(SensorId::GYROSCOPE, {.report_interval_us = 5'000})
            .ok());
    ASSERT_TRUE(
        driver.enable_sensor(SensorId::GYROSCOPE, {.report_interval_us = 2'500})
            .ok());
    EXPECT_EQ(driver.enabled_count(), 1U);
    EXPECT_EQ(driver.desired_config(SensorId::GYROSCOPE)->report_interval_us,
              2'500U);
}

TEST(Bno085, DisableDropsDesireAndSendsZeroInterval) {
    FakeHub hub;
    Bno085 driver(hub);
    drive_to_ready(hub, driver);
    ASSERT_TRUE(
        driver.enable_sensor(SensorId::GRAVITY, {.report_interval_us = 10'000})
            .ok());
    ASSERT_TRUE(driver.disable_sensor(SensorId::GRAVITY).ok());
    driver.service();  // flush the enable + disable cargoes
    EXPECT_EQ(driver.enabled_count(), 0U);
    EXPECT_EQ(driver.desired_config(SensorId::GRAVITY), nullptr);

    const auto& writes = hub.writes();
    ASSERT_FALSE(writes.empty());
    const auto& last = writes.back();
    EXPECT_EQ(write_report_id(last), pico_sh2::SET_FEATURE_COMMAND);
    EXPECT_EQ(last.frame.at(5), static_cast<std::uint8_t>(SensorId::GRAVITY));
    // report_interval_us is 4 LE bytes at payload offset 5 (frame offset 9).
    EXPECT_EQ(last.frame.at(9), 0U);
    EXPECT_EQ(last.frame.at(10), 0U);
    EXPECT_EQ(last.frame.at(11), 0U);
    EXPECT_EQ(last.frame.at(12), 0U);
}

TEST(Bno085, DisablingAnUnknownSensorIsANoOp) {
    FakeHub hub;
    Bno085 driver(hub);
    drive_to_ready(hub, driver);
    const std::size_t writes_before = hub.writes().size();
    EXPECT_TRUE(driver.disable_sensor(SensorId::STEP_COUNTER).ok());
    EXPECT_EQ(hub.writes().size(), writes_before);
}

// A full TX queue preserves retryable desired state (BR-2).

// Fill the queue without servicing it, then enable one more sensor (BR-2).
// first_id avoids existing IDs. Return the final sensor's ID.
std::uint8_t fill_tx_queue(Bno085& driver, std::uint8_t first_id) {
    const std::size_t depth = pico_sh2::ActiveProfile::TX_QUEUE_DEPTH;
    std::uint8_t overflowed = first_id;
    for (std::size_t i = 0; i <= depth; ++i) {
        auto id = static_cast<std::uint8_t>(first_id + i);
        auto result = driver.enable_sensor(static_cast<SensorId>(id),
                                           {.report_interval_us = 10'000});
        overflowed = id;
        // A recorded configuration with a full queue returns DEFERRED.
        if (!result.ok()) {
            ADD_FAILURE() << "enable_sensor failed rather than deferring: "
                          << static_cast<int>(result.error());
            break;
        }
        if (i == depth) {
            EXPECT_EQ(result.value(), Bno085::EnableState::DEFERRED);
        }
    }
    return overflowed;
}

TEST(Bno085, QueueFullEnableIsRescheduledAndEventuallySent) {
    FakeHub hub;
    Bno085 driver(hub);
    drive_to_ready(hub, driver);

    // The deferred configuration has not reached the hub yet.
    const std::uint8_t refused = fill_tx_queue(driver, 0x10);
    ASSERT_NE(driver.desired_config(static_cast<SensorId>(refused)), nullptr);
    EXPECT_EQ(count_set_feature(hub, static_cast<SensorId>(refused)), 0);

    // Service calls drain the queue and send the deferred configuration.
    for (int i = 0; i < 20; ++i) {
        driver.service();
    }
    EXPECT_GE(count_set_feature(hub, static_cast<SensorId>(refused)), 1);
}

TEST(Bno085, DisableKeepsDesiredEntryWhenQueueFull) {
    FakeHub hub;
    Bno085 driver(hub);
    drive_to_ready(hub, driver);
    ASSERT_TRUE(
        driver.enable_sensor(SensorId::GRAVITY, {.report_interval_us = 10'000})
            .ok());
    driver.service();  // flush the enable

    fill_tx_queue(driver, 0x10);
    const Result<void> result = driver.disable_sensor(SensorId::GRAVITY);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error(), Error::NO_ROOM);
    EXPECT_NE(driver.desired_config(SensorId::GRAVITY), nullptr);
}

// A new sensor at capacity evicts no entry (BR-1).

TEST(Bno085, EnableCapRejectsNewSensorsWithoutEviction) {
    FakeHub hub;
    Bno085 driver(hub);
    for (std::size_t i = 0; i < MAX_ENABLED_SENSORS; ++i) {
        auto sensor = static_cast<SensorId>(0x01 + i);
        ASSERT_TRUE(
            driver.enable_sensor(sensor, {.report_interval_us = 10'000}).ok());
    }
    EXPECT_EQ(driver.enabled_count(), MAX_ENABLED_SENSORS);

    // A new sensor beyond capacity is rejected without eviction (BR-1).
    auto overflow_sensor = static_cast<SensorId>(0x01 + MAX_ENABLED_SENSORS);
    auto result =
        driver.enable_sensor(overflow_sensor, {.report_interval_us = 10'000});
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error(), Error::UNSUPPORTED);
    EXPECT_EQ(driver.enabled_count(), MAX_ENABLED_SENSORS);
    EXPECT_EQ(driver.stats().config_rejections, 1U);

    EXPECT_TRUE(driver
                    .enable_sensor(static_cast<SensorId>(0x01),
                                   {.report_interval_us = 1'000})
                    .ok());
}

// Desired sensors are reapplied after hub resets (AR-3).

TEST(Bno085, ReEnablesDesiredSensorsAfterHubReset) {
    FakeHub hub;
    Bno085 driver(hub);
    ASSERT_TRUE(driver
                    .enable_sensor(SensorId::ROTATION_VECTOR,
                                   {.report_interval_us = 10'000})
                    .ok());
    ASSERT_TRUE(driver
                    .enable_sensor(SensorId::ACCELEROMETER,
                                   {.report_interval_us = 5'000})
                    .ok());
    drive_to_ready(hub, driver);
    driver.service();  // flush the initial apply
    EXPECT_EQ(count_set_feature(hub, SensorId::ROTATION_VECTOR), 1);
    EXPECT_EQ(count_set_feature(hub, SensorId::ACCELEROMETER), 1);

    // Readvertisement triggers the session handshake and driver reset
    // notification.
    hub.queue_transfer(frame(0, 1, advertisement()), 0);
    driver.service();
    EXPECT_FALSE(driver.ready());
    EXPECT_EQ(driver.stats().hub_resets, 1U);

    // Complete the handshake again; the driver re-applies both sensors (AR-3).
    hub.queue_transfer(frame(1, 1, {0x01}), 0);
    hub.queue_transfer(frame(2, 1, initialize_response(0)), 0);
    driver.service();
    ASSERT_TRUE(driver.ready());
    driver.service();  // flush the recovery re-apply

    EXPECT_EQ(count_set_feature(hub, SensorId::ROTATION_VECTOR), 2);
    EXPECT_EQ(count_set_feature(hub, SensorId::ACCELEROMETER), 2);
    const Stats stats = driver.stats();
    EXPECT_EQ(stats.recoveries, 1U);
    EXPECT_EQ(stats.sensors_reenabled, 2U);
}

TEST(Bno085, ReEnablesDesiredSensorsAfterAnUnacknowledgedRead) {
    FakeHub hub;
    Bno085 driver(hub);
    ASSERT_TRUE(driver
                    .enable_sensor(SensorId::ROTATION_VECTOR,
                                   {.report_interval_us = 10'000})
                    .ok());
    ASSERT_TRUE(driver
                    .enable_sensor(SensorId::ACCELEROMETER,
                                   {.report_interval_us = 5'000})
                    .ok());
    drive_to_ready(hub, driver);
    driver.service();  // flush the initial apply

    // A commanded reset can leave a signalled read unanswered. The driver
    // must not fault while recovering from the absent hub.
    hub.queue_transfer(frame(3, 0, {}), 0);
    hub.fail_next_read(pico_sh2::Sh2Error::BUS_ERROR);
    driver.service();
    EXPECT_FALSE(driver.ready());
    EXPECT_FALSE(driver.faulted());
    EXPECT_EQ(driver.stats().hub_resets, 1U);
    EXPECT_EQ(driver.counters().transport_restarts, 1U);

    // The hub returns and both sensors are re-applied (AR-3).
    drive_to_ready(hub, driver, 1);
    driver.service();  // flush the recovery re-apply
    EXPECT_EQ(count_set_feature(hub, SensorId::ROTATION_VECTOR), 2);
    EXPECT_EQ(count_set_feature(hub, SensorId::ACCELEROMETER), 2);
    const Stats stats = driver.stats();
    EXPECT_EQ(stats.recoveries, 1U);
    EXPECT_EQ(stats.sensors_reenabled, 2U);
}

TEST(Bno085, CommandForwardersRequireReady) {
    FakeHub hub;
    Bno085 driver(hub);
    EXPECT_EQ(driver.save_dcd().error(), Error::NOT_READY);
    EXPECT_EQ(driver.tare_now({}).error(), Error::NOT_READY);
}

TEST(Bno085, SaveDcdEmitsCommandRequest) {
    FakeHub hub;
    Bno085 driver(hub);
    drive_to_ready(hub, driver);
    ASSERT_TRUE(driver.save_dcd().ok());
    driver.service();  // flush the enqueued command request
    const auto& last = hub.writes().back();
    EXPECT_EQ(write_report_id(last), pico_sh2::COMMAND_REQUEST_REPORT);
    EXPECT_EQ(last.frame.at(6), pico_sh2::command::SAVE_DCD);  // cmd byte
}

// 1000-3625 6.4.7.1, Figure 59: P0 zero enables periodic DCD save.
TEST(Bno085, PeriodicDcdSaveSendsTheManualsEncoding) {
    for (const bool enable : {true, false}) {
        FakeHub hub;
        Bno085 driver(hub);
        drive_to_ready(hub, driver);
        ASSERT_TRUE(driver.set_periodic_dcd(enable).ok());
        driver.service();
        const auto& last = hub.writes().back();
        EXPECT_EQ(write_report_id(last), pico_sh2::COMMAND_REQUEST_REPORT);
        EXPECT_EQ(last.frame.at(7), enable ? 0x00U : 0x01U);
    }
}

// 1000-3625 6.4.7 and 6.4.9.1: the hub sends no response to these commands,
// so they neither hold the command slot nor raise a command timeout.
enum class ResponselessCommand : std::uint8_t {
    PERIODIC_DCD_ENABLE,
    PERIODIC_DCD_DISABLE,
    CLEAR_DCD_AND_RESET
};

constexpr std::array<ResponselessCommand, 3> RESPONSELESS_COMMANDS{
    ResponselessCommand::PERIODIC_DCD_ENABLE,
    ResponselessCommand::PERIODIC_DCD_DISABLE,
    ResponselessCommand::CLEAR_DCD_AND_RESET};

Result<void> send_responseless(Bno085& driver, ResponselessCommand command) {
    switch (command) {
        case ResponselessCommand::PERIODIC_DCD_ENABLE:
            return driver.set_periodic_dcd(true);
        case ResponselessCommand::PERIODIC_DCD_DISABLE:
            return driver.set_periodic_dcd(false);
        case ResponselessCommand::CLEAR_DCD_AND_RESET:
            break;
    }
    return driver.clear_dcd_and_reset();
}

TEST(Bno085, ResponselessCommandsDoNotHoldTheCommandSlot) {
    for (const ResponselessCommand command : RESPONSELESS_COMMANDS) {
        FakeHub hub;
        Bno085 driver(hub);
        drive_to_ready(hub, driver);
        ASSERT_TRUE(send_responseless(driver, command).ok());
        driver.service();
        EXPECT_TRUE(driver.save_dcd().ok());
    }
}

TEST(Bno085, ResponselessCommandsRaiseNoCommandTimeout) {
    for (const ResponselessCommand command : RESPONSELESS_COMMANDS) {
        FakeHub hub;
        Bno085 driver(hub);
        drive_to_ready(hub, driver);
        ASSERT_TRUE(send_responseless(driver, command).ok());
        driver.service();
        const auto diag_events = driver.stats().diag_events;
        hub.advance_time(pico_bno085::bno085_timing().command_timeout_us + 1);
        driver.service();
        EXPECT_EQ(driver.stats().diag_events, diag_events);
    }
}

TEST(Bno085, TareNowEmitsCommandWithParams) {
    FakeHub hub;
    Bno085 driver(hub);
    drive_to_ready(hub, driver);
    ASSERT_TRUE(
        driver
            .tare_now({.axes = Bno085::TareAxes::ALL,
                       .basis = Bno085::TareBasis::GEOMAGNETIC_ROTATION_VECTOR})
            .ok());
    driver.service();  // flush the enqueued command request
    const auto& last = hub.writes().back();
    EXPECT_EQ(write_report_id(last), pico_sh2::COMMAND_REQUEST_REPORT);
    EXPECT_EQ(last.frame.at(6), pico_sh2::command::TARE);  // cmd byte
    // Parameters follow the SHTP header and 0xF2, sequence and command bytes.
    EXPECT_EQ(last.frame.at(7), 0x00U);  // P0 = subcommand tare-now
    EXPECT_EQ(last.frame.at(8), 0x07U);  // P1 = axis bitmap
    EXPECT_EQ(last.frame.at(9), 0x02U);  // P2 = basis
}

TEST(Bno085, OnlyOneCommandOutstanding) {
    FakeHub hub;
    Bno085 driver(hub);
    drive_to_ready(hub, driver);
    ASSERT_TRUE(driver.save_dcd().ok());
    EXPECT_EQ(driver
                  .configure_dynamic_calibration(
                      {.accel = true, .gyro = true, .mag = true})
                  .error(),
              Error::BUSY);
}

// Driver counters record their documented events (OR-1).

TEST(Bno085, StatsMirrorSessionAndTrackEnabled) {
    FakeHub hub;
    Bno085 driver(hub);
    drive_to_ready(hub, driver);
    ASSERT_TRUE(driver
                    .enable_sensor(SensorId::ROTATION_VECTOR,
                                   {.report_interval_us = 10'000})
                    .ok());
    driver.service();  // flush the Set Feature so cargoes_out advances

    const Stats stats = driver.stats();
    EXPECT_EQ(stats.enabled_sensors, 1U);
    // The protocol counters come from the session, not a copy in Stats (OR-1).
    EXPECT_EQ(driver.counters().cargoes_out, 1U);
    EXPECT_GT(driver.counters().cargoes_in, 0U);
    EXPECT_EQ(stats.hub_resets, 0U);
    EXPECT_EQ(stats.recoveries, 0U);
}

// Every diagnostic event is counted (OR-2). The count must not share
// the log's rate limit.
TEST(Bno085, EveryDiagnosticEventIsCounted) {
    FakeHub hub;
    Bno085 driver(hub);
    drive_to_ready(hub, driver);
    ASSERT_EQ(driver.stats().diag_events, 0U);

    // An unanswered command expires with COMMAND_TIMEOUT.
    ASSERT_TRUE(driver.save_dcd().ok());
    driver.service();
    hub.advance_time(pico_bno085::bno085_timing().command_timeout_us + 1);
    driver.service();
    EXPECT_EQ(driver.stats().diag_events, 1U);

    // A repeated timeout counts even when no longer logged.
    ASSERT_TRUE(driver.save_dcd().ok());
    driver.service();
    hub.advance_time(pico_bno085::bno085_timing().command_timeout_us + 1);
    driver.service();
    EXPECT_EQ(driver.stats().diag_events, 2U);
}

// Reset injection preserves counter consistency (OR-1).

// Reproducible random enables, disables and resets exercise stats invariants.
// The weekly job selects this test with ctest -R Soak.
TEST(Bno085, SoakStatsRemainConsistentUnderResetInjection) {
    FakeHub hub;
    Bno085 driver(hub);
    drive_to_ready(hub, driver);

    // drive_to_ready consumed inbound sequence 0 on each channel.
    std::uint8_t adv_seq = 1;
    std::uint8_t exec_seq = 1;
    std::uint8_t ctrl_seq = 1;

    // The weekly job uses the same seed with more iterations.
    int iterations = 400;
    // concurrency-mt-unsafe: getenv reads once before the suite starts threads.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    if (const char* env = std::getenv("PICO_BNO085_SOAK_ITERATIONS")) {
        const std::string_view text{env};
        int parsed = 0;
        const auto [ptr, ec] =
            std::from_chars(text.data(), text.data() + text.size(), parsed);
        if (ec == std::errc{} && ptr == text.data() + text.size() &&
            parsed > 0) {
            iterations = parsed;
        }
    }

    // cert-msc51-cpp: a fixed seed makes a soak failure reproducible.
    // NOLINTNEXTLINE(cert-msc51-cpp)
    std::mt19937 rng(0xB0085);
    auto pick_sensor = [&]() {
        static constexpr std::array<SensorId, 6> SENSORS = {
            SensorId::ACCELEROMETER,  SensorId::GYROSCOPE,
            SensorId::MAGNETIC_FIELD, SensorId::ROTATION_VECTOR,
            SensorId::GRAVITY,        SensorId::LINEAR_ACCELERATION};
        return SENSORS[rng() % SENSORS.size()];
    };

    std::uint32_t injected_resets = 0;
    for (int i = 0; i < iterations; ++i) {
        const int action = static_cast<int>(rng() % 10);
        if (action < 4) {
            // A full queue is an expected random outcome. Later assertions
            // check recovery after the deferred enable (BR-2).
            static_cast<void>(driver.enable_sensor(
                pick_sensor(),
                {.report_interval_us =
                     1'000 +
                     (static_cast<std::uint32_t>(rng() % 20) * 1'000)}));
        } else if (action < 7) {
            static_cast<void>(driver.disable_sensor(pick_sensor()));
        } else if (action < 9) {
            driver.service();  // flush any pending cargoes
        } else {
            hub.queue_transfer(frame(0, adv_seq++, advertisement()), 0);
            driver.service();
            hub.queue_transfer(frame(1, exec_seq++, {0x01}), 0);
            hub.queue_transfer(frame(2, ctrl_seq++, initialize_response(0)), 0);
            driver.service();
            ++injected_resets;
        }
        driver.service();

        const Stats stats = driver.stats();
        ASSERT_FALSE(driver.faulted());
        ASSERT_LE(stats.recoveries, stats.hub_resets);
        ASSERT_LE(stats.enabled_sensors, MAX_ENABLED_SENSORS);
        ASSERT_EQ(stats.enabled_sensors, driver.enabled_count());
    }

    // Each injected reset completes its handshake in the loop.
    EXPECT_EQ(driver.stats().hub_resets, injected_resets);
    EXPECT_EQ(driver.stats().recoveries, injected_resets);
    EXPECT_TRUE(driver.ready());
}

// Sensor options encode the feature flags byte (1000-3625 §6.5.2).
TEST(Bno085, SensorOptionsBecomeTheFeatureFlags) {
    const Bno085::SensorOptions plain;
    EXPECT_EQ(plain.flags(), 0U);

    Bno085::SensorOptions wake;
    wake.wake = true;
    EXPECT_EQ(wake.flags(), pico_sh2::feature_flag::WAKE);

    Bno085::SensorOptions everything;
    everything.wake = true;
    everything.always_on = true;
    everything.change_sensitivity_enabled = true;
    everything.change_sensitivity_relative = true;
    EXPECT_EQ(everything.flags(),
              static_cast<unsigned>(pico_sh2::feature_flag::WAKE) |
                  static_cast<unsigned>(pico_sh2::feature_flag::ALWAYS_ON) |
                  static_cast<unsigned>(
                      pico_sh2::feature_flag::CHANGE_SENSITIVITY_ENABLED) |
                  static_cast<unsigned>(
                      pico_sh2::feature_flag::CHANGE_SENSITIVITY_RELATIVE));
}

TEST(Bno085, WakeOptionReachesTheHubInTheSetFeature) {
    FakeHub hub;
    Bno085 driver(hub);
    drive_to_ready(hub, driver);

    Bno085::SensorOptions options;
    options.report_interval_us = 10'000;
    options.batch_interval_us = 100'000;
    options.wake = true;
    ASSERT_TRUE(driver.enable_sensor(SensorId::ACCELEROMETER, options).ok());
    driver.service();

    bool found = false;
    for (const auto& write : hub.writes()) {
        const auto& frame = write.frame;
        if (frame.size() < pico_sh2::shtp::HEADER_SIZE + 17) {
            continue;
        }
        const std::uint8_t* body = frame.data() + pico_sh2::shtp::HEADER_SIZE;
        if (body[0] != pico_sh2::SET_FEATURE_COMMAND ||
            body[1] != static_cast<std::uint8_t>(SensorId::ACCELEROMETER)) {
            continue;
        }
        found = true;
        EXPECT_EQ(body[2] & pico_sh2::feature_flag::WAKE,
                  pico_sh2::feature_flag::WAKE);
        EXPECT_EQ(body[5], 0x10U);  // 10000 us, LSB first
        EXPECT_EQ(body[6], 0x27U);
    }
    EXPECT_TRUE(found);
}

// Calibration survival across executable-channel reset is UNCONFIRMED (FW-2).
// A handshake completed within one service() does not expose a not-ready edge.
TEST(Bno085, ResetRestartsTheHandshakeAndReEnablesSensors) {
    FakeHub hub;
    Bno085 driver(hub);
    drive_to_ready(hub, driver);
    ASSERT_TRUE(driver
                    .enable_sensor(SensorId::ROTATION_VECTOR,
                                   {.report_interval_us = 10'000})
                    .ok());
    driver.service();

    ASSERT_TRUE(driver.reset().ok());
    driver.service();

    // Readvertisement triggers recovery and sensor reapplication (AR-3).
    drive_to_ready(hub, driver, /*re_seq=*/1);
    driver.service();

    EXPECT_EQ(driver.stats().hub_resets, 1U);
    EXPECT_EQ(driver.stats().recoveries, 1U);
    EXPECT_GT(driver.stats().sensors_reenabled, 0U);
    EXPECT_EQ(driver.enabled_count(), 1U);
    // Check actual Set Feature traffic before and after reset.
    EXPECT_EQ(count_set_feature(hub, SensorId::ROTATION_VECTOR), 2);
}

TEST(Bno085, OscillatorAndErrorCommandsRequireReady) {
    FakeHub hub;
    Bno085 driver(hub);
    EXPECT_EQ(driver.request_oscillator_type().error(), Error::NOT_READY);
    EXPECT_EQ(driver.report_errors().error(), Error::NOT_READY);
}

}  // namespace
}  // namespace pico_bno085
