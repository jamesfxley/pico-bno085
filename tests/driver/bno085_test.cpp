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

// --- SHTP frame builders (mirroring the pico-sh2 session-test fixtures) ------

std::vector<std::uint8_t> frame(std::uint8_t channel, std::uint8_t seq,
                                std::vector<std::uint8_t> payload) {
    const std::size_t total = payload.size() + 4;
    std::vector<std::uint8_t> out;
    // Also what lets GCC 13 see the capacity: without it, _FORTIFY_SOURCE=3 at
    // -O3 cannot bound the insert below and reports a stringop-overflow.
    out.reserve(total);
    out.push_back(static_cast<std::uint8_t>(total & 0xFFU));
    out.push_back(static_cast<std::uint8_t>((total >> 8U) & 0x7FU));
    out.push_back(channel);
    out.push_back(seq);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// A minimal but valid channel-0 advertisement: SHTP Response 0 (1000-3535
// §5.1.1.1), then the sensorhub GUID 2 and one tag-0x81 report-length pair.
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

// Drive the driver from construction to READY. `re_seq` selects the per-channel
// sequence number, so the same helper drives a post-reset recovery.
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

// The report ID (byte 0 of the SHTP payload) of a captured host write.
std::uint8_t write_report_id(const FakeHub::CapturedWrite& write) {
    return write.frame.at(4);
}

// Count captured writes that are Set Feature (0xFD) for `sensor`.
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

// --- the driver's own error taxonomy (AR-4) ---------------------------------

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
    // Four SHTP/session codes, one recovery: reset the hub. A caller offered
    // the difference has nothing to do with it.
    EXPECT_EQ(from_sh2(pico_sh2::Sh2Error::BAD_HEADER), Error::PROTOCOL);
    EXPECT_EQ(from_sh2(pico_sh2::Sh2Error::CARGO_TOO_LARGE), Error::PROTOCOL);
    EXPECT_EQ(from_sh2(pico_sh2::Sh2Error::REASSEMBLY_DROP), Error::PROTOCOL);
    EXPECT_EQ(from_sh2(pico_sh2::Sh2Error::PROTOCOL), Error::PROTOCOL);

    // Both capacity codes are "retry once something drains" (BR-2).
    EXPECT_EQ(from_sh2(pico_sh2::Sh2Error::QUEUE_FULL), Error::NO_ROOM);
    EXPECT_EQ(from_sh2(pico_sh2::Sh2Error::TABLE_FULL), Error::NO_ROOM);
}

TEST(Bno085Error, SuccessIsZeroSoResultOkRestsOnIt) {
    // pico_util::Result compares the error against E{}, so NONE must be the
    // value-initialised enumerator or every Result reads as failed.
    EXPECT_EQ(static_cast<std::uint8_t>(Error{}), 0U);
    EXPECT_EQ(Error{}, Error::NONE);
    EXPECT_TRUE(Result<void>{}.ok());
}

// --- lifecycle (TR-1) --------------------------------------------------------

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

// TR-1: the two later phases fault the same way, each recording where it was.
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

// HR-1: a transport that reports no data is never read. The decorator counts
// the reads the driver issues through it, which is what a blind poll would
// raise; a device run cannot tell a blind poll of a present hub from silence.
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

    // And the gate opens: with a transfer waiting, the next service reads it.
    hub.queue_idle_transfer();
    driver.service();
    EXPECT_EQ(counting.reads(), at_ready + 1);
}

// --- sensor surface + re-enable policy (AR-3) -------------------------------

// A caller waiting to see events needs to know whether the hub has been told
// yet. Both outcomes are ordinary, so neither is an error.
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

// BR-2: a full queue defers rather than fails, because the desire is recorded
// and a re-apply is scheduled.
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

    // The deferred request reaches the hub once the queue drains.
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
    // Enabling before READY records desire but sends nothing yet.
    ASSERT_TRUE(driver
                    .enable_sensor(SensorId::ROTATION_VECTOR,
                                   {.report_interval_us = 10'000})
                    .ok());
    EXPECT_EQ(count_set_feature(hub, SensorId::ROTATION_VECTOR), 0);

    drive_to_ready(hub, driver);
    // Reaching READY enqueues the desired sensor; the next service() flushes
    // it.
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

    // The last Gravity write is a zero-interval (disabling) Set Feature.
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

// --- TX-queue backpressure (BR-2) -------------------------------------------

// Fill the session TX queue while READY by enabling sensors without letting
// service() flush it, then enable one more. `first_id` picks a sensor-ID base
// that avoids colliding with other sensors a test has enabled. Returns the ID
// of that last sensor, whose immediate send the full queue refused.
//
// The overflow is the iteration past the depth: `depth` enables fill the ring
// exactly and every one of them is accepted, so a loop that stops there never
// reaches the case (BR-2).
std::uint8_t fill_tx_queue(Bno085& driver, std::uint8_t first_id) {
    const std::size_t depth = pico_sh2::ActiveProfile::TX_QUEUE_DEPTH;
    std::uint8_t overflowed = first_id;
    for (std::size_t i = 0; i <= depth; ++i) {
        auto id = static_cast<std::uint8_t>(first_id + i);
        auto result = driver.enable_sensor(static_cast<SensorId>(id),
                                           {.report_interval_us = 10'000});
        overflowed = id;
        // A full queue is not an error here: the desired entry is recorded and
        // a re-apply scheduled, so the call reports DEFERRED rather than
        // NO_ROOM.
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

    // The overflowing enable is recorded and deferred, so nothing of it has
    // reached the hub yet.
    const std::uint8_t refused = fill_tx_queue(driver, 0x10);
    ASSERT_NE(driver.desired_config(static_cast<SensorId>(refused)), nullptr);
    EXPECT_EQ(count_set_feature(hub, static_cast<SensorId>(refused)), 0);

    // Draining service() calls flush the queue and run the scheduled re-apply,
    // so the refused sensor's Set Feature does reach the hub (contract).
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

    // Fill the queue so the disable's zero-interval Set Feature cannot enqueue.
    fill_tx_queue(driver, 0x10);
    const Result<void> result = driver.disable_sensor(SensorId::GRAVITY);
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error(), Error::NO_ROOM);
    // The sensor is kept (not silently dropped), so the caller can retry.
    EXPECT_NE(driver.desired_config(SensorId::GRAVITY), nullptr);
}

// --- enabled-sensor cap (BR-1) ----------------------------------------------

TEST(Bno085, EnableCapRejectsNewSensorsWithoutEviction) {
    FakeHub hub;
    Bno085 driver(hub);
    // Fill the desired table to the profile cap.
    for (std::size_t i = 0; i < MAX_ENABLED_SENSORS; ++i) {
        auto sensor = static_cast<SensorId>(0x01 + i);
        ASSERT_TRUE(
            driver.enable_sensor(sensor, {.report_interval_us = 10'000}).ok());
    }
    EXPECT_EQ(driver.enabled_count(), MAX_ENABLED_SENSORS);

    // One more distinct sensor is refused (BR-1) and evicts nothing.
    auto overflow_sensor = static_cast<SensorId>(0x01 + MAX_ENABLED_SENSORS);
    auto result =
        driver.enable_sensor(overflow_sensor, {.report_interval_us = 10'000});
    EXPECT_FALSE(result.ok());
    EXPECT_EQ(result.error(), Error::UNSUPPORTED);
    EXPECT_EQ(driver.enabled_count(), MAX_ENABLED_SENSORS);
    EXPECT_EQ(driver.stats().config_rejections, 1U);

    // Reconfiguring an already-desired sensor at the cap still succeeds.
    EXPECT_TRUE(driver
                    .enable_sensor(static_cast<SensorId>(0x01),
                                   {.report_interval_us = 1'000})
                    .ok());
}

// --- reset recovery re-enable (AR-3) ----------------------------------------

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

    // The hub spontaneously resets (re-advertises). The session restarts its
    // handshake; the driver notes the reset.
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

    // A commanded reset takes the hub off the bus mid-stream, so a read it had
    // already signalled goes unanswered. That is the same reset the driver
    // recovers from above, reaching it by the transport rather than by a
    // re-advertisement, and it must not leave the driver faulted.
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

// --- calibration / DCD forwarders -------------------------------------------

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
    // Request layout: 0xF2, seq, cmd, P0..P8 -> frame offset 4 +
    // [0xF2,seq,cmd].
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

// --- stats (OR-1) -----------------------------------------------------------

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

// OR-2: every diagnostic event is counted. Logging them is rate limited so a
// storm cannot put a log line in service() once per event, but the count is
// what a device run reads, so it must not be rate limited too.
TEST(Bno085, EveryDiagnosticEventIsCounted) {
    FakeHub hub;
    Bno085 driver(hub);
    drive_to_ready(hub, driver);
    ASSERT_EQ(driver.stats().diag_events, 0U);

    // A command the hub never answers: the deadline expires and the session
    // reports COMMAND_TIMEOUT.
    ASSERT_TRUE(driver.save_dcd().ok());
    driver.service();
    hub.advance_time(pico_bno085::bno085_timing().command_timeout_us + 1);
    driver.service();
    EXPECT_EQ(driver.stats().diag_events, 1U);

    // A second timeout of the same kind still counts, though it is not logged
    // again.
    ASSERT_TRUE(driver.save_dcd().ok());
    driver.service();
    hub.advance_time(pico_bno085::bno085_timing().command_timeout_us + 1);
    driver.service();
    EXPECT_EQ(driver.stats().diag_events, 2U);
}

// --- randomized soak (OR-1 self-consistency under fault injection) -----------

// Churn the driver: random enable/disable of a small sensor set interleaved
// with injected hub resets, asserting the driver's stats invariants hold at
// every step. Deterministic (fixed seed) so a failure reproduces. Selected by
// name -- `ctest -R Soak` -- rather than by a ctest label, which is why the
// name carries the word.
TEST(Bno085, SoakStatsRemainConsistentUnderResetInjection) {
    FakeHub hub;
    Bno085 driver(hub);
    drive_to_ready(hub, driver);

    // Per-channel inbound sequence numbers; seq 0 was consumed by
    // drive_to_ready.
    std::uint8_t adv_seq = 1;
    std::uint8_t exec_seq = 1;
    std::uint8_t ctrl_seq = 1;

    // The weekly job runs this far longer than a per-push run can afford.
    // Deterministic either way: the same seed, just more of it.
    int iterations = 400;
    // getenv is flagged as thread-unsafe. This runs once at the top of a test
    // body, before anything in the suite starts a thread, and the value is read
    // rather than modified.
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

    // A constant seed is the point: the soak has to replay identically when it
    // fails, and the nightly job differs from a per-push run only in how many
    // iterations it takes, not in which ones.
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
            // Discarded deliberately: this soak drives the state machine with
            // random traffic, and a refused enable is one of the outcomes under
            // test rather than a failure of it -- BR-2 makes queue-full
            // recoverable, and the assertions after the loop are what check the
            // driver came back. The cast says so at the site.
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
            // Inject a spontaneous hub reset and complete the handshake again.
            hub.queue_transfer(frame(0, adv_seq++, advertisement()), 0);
            driver.service();
            hub.queue_transfer(frame(1, exec_seq++, {0x01}), 0);
            hub.queue_transfer(frame(2, ctrl_seq++, initialize_response(0)), 0);
            driver.service();
            ++injected_resets;
        }
        driver.service();

        const Stats stats = driver.stats();
        // The driver never faults on well-formed traffic.
        ASSERT_FALSE(driver.faulted());
        // Recoveries can never outrun the resets that triggered them.
        ASSERT_LE(stats.recoveries, stats.hub_resets);
        // The desired table stays within the cap and matches the live count.
        ASSERT_LE(stats.enabled_sensors, MAX_ENABLED_SENSORS);
        ASSERT_EQ(stats.enabled_sensors, driver.enabled_count());
    }

    // Every injected reset was observed, and each was recovered (the loop
    // completes the handshake immediately after injecting).
    EXPECT_EQ(driver.stats().hub_resets, injected_resets);
    EXPECT_EQ(driver.stats().recoveries, injected_resets);
    EXPECT_TRUE(driver.ready());
}

// 1000-3625 §6.5.2: the options a caller means, turned into the flags byte the
// feature report carries.
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

    // Find the Set Feature and check the flags byte and both intervals.
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

// A restart using the executable-channel command; calibration survival is
// UNCONFIRMED.
//
// The whole re-handshake is drained inside one service() here, so the session
// never appears not-ready to the driver. Keying the re-apply on that edge alone
// would leave the hub streaming nothing while the desired table said otherwise.
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

    // The hub restarts and re-announces itself; the driver recovers and
    // re-applies what was asked for (AR-3).
    drive_to_ready(hub, driver, /*re_seq=*/1);
    driver.service();

    EXPECT_EQ(driver.stats().hub_resets, 1U);
    EXPECT_EQ(driver.stats().recoveries, 1U);
    EXPECT_GT(driver.stats().sensors_reenabled, 0U);
    EXPECT_EQ(driver.enabled_count(), 1U);
    // Re-applied to the hub, not only counted: once before the reset, once
    // after it.
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
