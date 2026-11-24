// SPDX-License-Identifier: MIT
// The bench line protocol: payload off prints what the bench always printed
// (LP-4, LP-5), payload lines have the specified fields and number format
// (LP-6), commands parse as specified (LP-2) and their responses print as
// lines (LP-7). The bandwidth bound is LP-8.
#include "bench_protocol.hpp"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "pico_bno085/driver/bno085.hpp"
#include "pico_sh2/reports/report_layout.hpp"
#include "pico_sh2/reports/sensor_event.hpp"
#include "pico_sh2/reports/sensor_id.hpp"
#include "pico_sh2/sh2/command.hpp"

namespace {

using pico_bno085::Bno085;
using pico_bno085::bench::format_command_response;
using pico_bno085::bench::format_event;
using pico_bno085::bench::LINE_BUFFER_SIZE;
using pico_sh2::CommandResponse;
using pico_sh2::reports::EventPayload;
using pico_sh2::reports::GyroIntegratedSample;
using pico_sh2::reports::Quaternion;
using pico_sh2::reports::RawSample;
using pico_sh2::reports::RotationSample;
using pico_sh2::reports::SensorAccuracy;
using pico_sh2::reports::SensorEvent;
using pico_sh2::reports::SensorId;
using pico_sh2::reports::StepCountSample;
using pico_sh2::reports::TapSample;
using pico_sh2::reports::UncalibratedSample;
using pico_sh2::reports::Vec3;

Vec3 vec3(float x_value, float y_value, float z_value) {
    return {.x = x_value, .y = y_value, .z = z_value};
}

SensorEvent make_event(SensorId sensor, EventPayload payload) {
    SensorEvent event;
    event.sensor = sensor;
    event.sequence = 7;
    event.accuracy = SensorAccuracy::HIGH;
    event.event_us = 1'234'567;
    event.payload = payload;
    return event;
}

std::string line_of(const SensorEvent& event, bool payload) {
    std::array<char, LINE_BUFFER_SIZE> buffer{};
    const std::size_t length = format_event(buffer, event, payload);
    return {buffer.data(), length};
}

std::string fixed(float value) {
    std::array<char, LINE_BUFFER_SIZE> buffer{};
    pico_bno085::bench::LineWriter writer(buffer);
    writer.put_fixed(value);
    const std::size_t length = writer.finish();
    return {buffer.data(), length};
}

// An independent reference for the integer formatter, built on a double
// product.
std::string reference_fixed(float value) {
    const auto wide = static_cast<double>(value);
    if (!std::isfinite(wide) || std::fabs(wide) >= 1.0e9) {
        return "nan";
    }
    const std::int64_t scaled = std::llround(wide * 100000.0);
    std::string text;
    if (scaled < 0) {
        text += '-';
    }
    constexpr std::uint64_t SCALE = 100'000;
    constexpr std::size_t PLACES = 5;
    const auto magnitude =
        static_cast<std::uint64_t>(scaled < 0 ? -scaled : scaled);
    text += std::to_string(magnitude / SCALE);
    text += '.';
    const std::string fraction = std::to_string(magnitude % SCALE);
    text.append(PLACES - fraction.size(), '0');
    text += fraction;
    return text;
}

// What the bench printed for every event before payload mode existed:
// `printf("E,%u,%u,%llu,%u\n", ...)`, each field in decimal.
std::string legacy_line(const SensorEvent& event) {
    return "E," + std::to_string(static_cast<unsigned>(event.sensor)) + "," +
           std::to_string(static_cast<unsigned>(event.sequence)) + "," +
           std::to_string(event.event_us) + "," +
           std::to_string(static_cast<unsigned>(event.accuracy)) + "\n";
}

std::array<EventPayload, 11> every_kind() {
    return {
        EventPayload{},
        vec3(1.5F, -2.25F, 3.0F),
        UncalibratedSample{.value = vec3(1.0F, 2.0F, 3.0F),
                           .bias = vec3(0.5F, -0.5F, 0.25F)},
        RotationSample{.unit = {.i = 0.1F, .j = 0.2F, .k = 0.3F, .real = 0.9F},
                       .accuracy_rad = 0.05F},
        Quaternion{.i = 0.1F, .j = 0.2F, .k = 0.3F, .real = 0.9F},
        GyroIntegratedSample{
            .unit = {.i = 0.1F, .j = 0.2F, .k = 0.3F, .real = 0.9F},
            .angular_velocity = vec3(1.0F, 2.0F, 3.0F)},
        TapSample{.taps = 3},
        StepCountSample{.latency_us = 5, .steps = 9},
        pico_sh2::reports::StepDetectSample{.latency_us = 5},
        pico_sh2::reports::StabilitySample{.classification = 2},
        RawSample{.x = -1, .y = 2, .z = -3, .timestamp = 4},
    };
}

// Payload off must print the legacy line for every kind of sample, whatever
// the header fields hold (LP-4, LP-5).
TEST(BenchProtocol, PayloadOffPrintsTheLegacyLineForEveryKindAndHeader) {
    const std::array<std::uint64_t, 4> times{
        0, 1, 1'234'567, std::numeric_limits<std::uint64_t>::max()};
    for (const EventPayload& payload : every_kind()) {
        for (const std::uint8_t sensor :
             std::array<std::uint8_t, 4>{1, 5, 42, 255}) {
            for (const std::uint64_t time : times) {
                for (const auto accuracy :
                     {SensorAccuracy::UNRELIABLE, SensorAccuracy::LOW,
                      SensorAccuracy::MEDIUM, SensorAccuracy::HIGH}) {
                    SensorEvent event =
                        make_event(static_cast<SensorId>(sensor), payload);
                    event.event_us = time;
                    event.accuracy = accuracy;
                    event.sequence = static_cast<std::uint8_t>(sensor + 1);
                    EXPECT_EQ(line_of(event, false), legacy_line(event));
                }
            }
        }
    }
}

TEST(BenchProtocol, PayloadOnKeepsELinesForKindsWithoutAPayloadLine) {
    const auto kinds = every_kind();
    for (const std::size_t index :
         {std::size_t{0}, std::size_t{6}, std::size_t{7}, std::size_t{8},
          std::size_t{9}, std::size_t{10}}) {
        const SensorEvent event =
            make_event(SensorId::TAP_DETECTOR, kinds[index]);
        EXPECT_EQ(line_of(event, true), legacy_line(event)) << index;
    }
}

TEST(BenchProtocol, RotationVectorLineIsQuaternionThenAccuracy) {
    const SensorEvent event = make_event(
        SensorId::ROTATION_VECTOR,
        RotationSample{
            .unit = {.i = 0.25F, .j = -0.5F, .k = 0.125F, .real = 0.75F},
            .accuracy_rad = 0.0625F});
    EXPECT_EQ(line_of(event, true),
              "Q,5,7,1234567,3,0.75000,0.25000,-0.50000,0.12500,0.06250\n");
}

TEST(BenchProtocol, GameRotationLineHasNoAccuracyField) {
    const SensorEvent event = make_event(
        SensorId::GAME_ROTATION_VECTOR,
        Quaternion{.i = 0.25F, .j = -0.5F, .k = 0.125F, .real = 0.75F});
    EXPECT_EQ(line_of(event, true),
              "R,8,7,1234567,3,0.75000,0.25000,-0.50000,0.12500\n");
}

TEST(BenchProtocol, VectorLineIsXyz) {
    const SensorEvent event =
        make_event(SensorId::ACCELEROMETER, vec3(1.5F, -2.25F, 9.8125F));
    EXPECT_EQ(line_of(event, true),
              "V,1,7,1234567,3,1.50000,-2.25000,9.81250\n");
}

TEST(BenchProtocol, UncalibratedLineIsValueThenBias) {
    const SensorEvent event =
        make_event(SensorId::GYROSCOPE_UNCALIBRATED,
                   UncalibratedSample{.value = vec3(1.0F, 2.0F, 3.0F),
                                      .bias = vec3(0.5F, -0.5F, 0.25F)});
    EXPECT_EQ(
        line_of(event, true),
        "U,7,7,1234567,3,1.00000,2.00000,3.00000,0.50000,-0.50000,0.25000\n");
}

TEST(BenchProtocol, GyroIntegratedLineIsQuaternionThenAngularVelocity) {
    const SensorEvent event = make_event(
        SensorId::GYRO_INTEGRATED_ROTATION_VECTOR,
        GyroIntegratedSample{
            .unit = {.i = 0.25F, .j = -0.5F, .k = 0.125F, .real = 0.75F},
            .angular_velocity = vec3(1.0F, -2.0F, 0.5F)});
    EXPECT_EQ(line_of(event, true),
              "G,42,7,1234567,3,0.75000,0.25000,-0.50000,0.12500,"
              "1.00000,-2.00000,0.50000\n");
}

// The sensor ID selects nothing: the sample's kind does (LP-5).
TEST(BenchProtocol, TheKindOfSampleSelectsTheLine) {
    const SensorEvent event =
        make_event(SensorId::GEOMAGNETIC_ROTATION_VECTOR,
                   RotationSample{.unit = {.i = 0, .j = 0, .k = 0, .real = 1},
                                  .accuracy_rad = 0});
    EXPECT_EQ(line_of(event, true),
              "Q,9,7,1234567,3,1.00000,0.00000,0.00000,0.00000,0.00000\n");
}

TEST(BenchProtocol, FixedNumbersHaveFiveDigitsAndRoundToNearest) {
    EXPECT_EQ(fixed(0.0F), "0.00000");
    EXPECT_EQ(fixed(1.0F), "1.00000");
    EXPECT_EQ(fixed(-1.0F), "-1.00000");
    EXPECT_EQ(fixed(0.000016F), "0.00002");
    EXPECT_EQ(fixed(-0.000016F), "-0.00002");
    EXPECT_EQ(fixed(0.000014F), "0.00001");
    EXPECT_EQ(fixed(0.000004F), "0.00000");
    EXPECT_EQ(fixed(2047.9375F), "2047.93750");
    EXPECT_EQ(fixed(-128.0F), "-128.00000");
}

TEST(BenchProtocol, ANegativeValueThatRoundsToZeroCarriesNoSign) {
    EXPECT_EQ(fixed(-0.000001F), "0.00000");
    EXPECT_EQ(fixed(-0.0F), "0.00000");
}

TEST(BenchProtocol, ANonFiniteValueIsNan) {
    EXPECT_EQ(fixed(std::numeric_limits<float>::quiet_NaN()), "nan");
    EXPECT_EQ(fixed(std::numeric_limits<float>::infinity()), "nan");
    EXPECT_EQ(fixed(-std::numeric_limits<float>::infinity()), "nan");
    EXPECT_EQ(fixed(2.0e9F), "nan");
}

TEST(BenchProtocol, FixedFormattingMatchesTheDoubleReferenceAtEdges) {
    EXPECT_EQ(fixed(0.015625F), "0.01563");
    EXPECT_EQ(fixed(-0.015625F), "-0.01563");
    EXPECT_EQ(fixed(1.5e-5F), "0.00001");
    EXPECT_EQ(fixed(std::nextafter(1.5e-5F, 1.0F)), "0.00002");
    EXPECT_EQ(fixed(-0.000004F), "0.00000");

    const std::array<float, 12> edges{
        0.0F,
        -0.0F,
        0.015625F,
        -0.015625F,
        1.5e-5F,
        std::nextafter(1.5e-5F, 1.0F),
        -0.000004F,
        std::bit_cast<float>(std::uint32_t{1}),
        std::bit_cast<float>(std::uint32_t{0x007F'FFFFU}),
        std::numeric_limits<float>::min(),
        std::nextafter(1.0e9F, 0.0F),
        1.0e9F,
    };
    for (const float value : edges) {
        EXPECT_EQ(fixed(value), reference_fixed(value)) << value;
    }
    const std::array<float, 4> non_finite{
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        std::nextafter(1.0e9F, 2.0e9F),
    };
    for (const float value : non_finite) {
        EXPECT_EQ(fixed(value), "nan") << value;
        EXPECT_EQ(reference_fixed(value), "nan") << value;
    }
}

TEST(BenchProtocol, FixedFormattingMatchesTheDoubleReferenceForRandomBits) {
    constexpr std::uint32_t PATTERNS = 100'000;
    constexpr std::uint32_t MULTIPLIER = 1'664'525U;
    constexpr std::uint32_t INCREMENT = 1'013'904'223U;
    constexpr std::uint32_t SIGN_BIT = 0x8000'0000U;
    constexpr std::uint32_t FRACTION_MASK = 0x007F'FFFFU;
    constexpr unsigned EXPONENT_BITS = 23;
    constexpr std::uint32_t EXPONENT_COUNT = 256;
    // The low bits of a power-of-two LCG repeat quickly; folding in the high
    // bits varies the fraction across exponents.
    constexpr unsigned MIX_SHIFT = 16;
    std::uint32_t state = 0x1234'5678U;
    for (std::uint32_t index = 0; index < PATTERNS; ++index) {
        state = state * MULTIPLIER + INCREMENT;
        const std::uint32_t mixed = state ^ (state >> MIX_SHIFT);
        const std::uint32_t exponent = index % EXPONENT_COUNT;
        const std::uint32_t bits = (mixed & SIGN_BIT) |
                                   (exponent << EXPONENT_BITS) |
                                   (mixed & FRACTION_MASK);
        const auto value = std::bit_cast<float>(bits);
        EXPECT_EQ(fixed(value), reference_fixed(value)) << bits;
    }
}

TEST(BenchProtocol, ABufferThatIsTooSmallYieldsNoLine) {
    const SensorEvent event =
        make_event(SensorId::ACCELEROMETER, vec3(1.0F, 2.0F, 3.0F));
    std::array<char, 8> small{};
    EXPECT_EQ(format_event(small, event, true), 0U);
    const std::string expected = line_of(event, true);
    std::array<char, LINE_BUFFER_SIZE> exact{};
    EXPECT_EQ(format_event(std::span<char>(exact.data(), expected.size() + 1),
                           event, true),
              expected.size());
    EXPECT_EQ(format_event(std::span<char>(exact.data(), expected.size()),
                           event, true),
              0U);
}

std::string response_line(const CommandResponse& response) {
    std::array<char, LINE_BUFFER_SIZE> buffer{};
    const std::size_t length = format_command_response(buffer, response);
    return {buffer.data(), length};
}

TEST(BenchProtocol, SaveDcdResponseReportsItsStatus) {
    CommandResponse response;
    response.command = pico_sh2::command::SAVE_DCD;
    response.results[0] = 1;
    EXPECT_EQ(response_line(response), "# dcd status=1\n");
}

TEST(BenchProtocol, MeCalibrationResponseReportsEachRoutine) {
    CommandResponse response;
    response.command = pico_sh2::command::ME_CALIBRATION;
    response.results = {0, 1, 0, 1, 0, 1};
    EXPECT_EQ(response_line(response),
              "# cal status=0 accel=1 gyro=0 mag=1 planar=0 table=1\n");
}

TEST(BenchProtocol, OtherResponsesPrintTheirRawResults) {
    CommandResponse response;
    response.command = pico_sh2::command::TARE;
    response.command_sequence = 12;
    response.response_sequence = 1;
    response.results = {0x00, 0xAB, 0xFF, 0x01, 0, 0, 0, 0, 0, 0, 0x7E};
    EXPECT_EQ(response_line(response),
              "# resp cmd=03 seq=12 rseq=1 r=00abff010000000000007e\n");
}

TEST(BenchProtocol, FlagArgumentsAreZeroOrOneAlone) {
    using pico_bno085::bench::parse_flag;
    EXPECT_EQ(parse_flag("0"), std::optional<bool>(false));
    EXPECT_EQ(parse_flag("1"), std::optional<bool>(true));
    EXPECT_EQ(parse_flag(" 1 "), std::optional<bool>(true));
    EXPECT_FALSE(parse_flag("2"));
    EXPECT_FALSE(parse_flag(""));
    EXPECT_FALSE(parse_flag("1 0"));
    EXPECT_FALSE(parse_flag("on"));
    EXPECT_FALSE(parse_flag("-1"));
}

TEST(BenchProtocol, TareTakesSupportedAxesAndABasis) {
    using pico_bno085::bench::parse_tare;
    const auto all = parse_tare("7 0");
    if (!all.has_value()) {
        FAIL() << "tare refused the X, Y and Z axes";
        return;
    }
    EXPECT_EQ(all->axes, Bno085::TareAxes::ALL);
    EXPECT_EQ(all->basis, Bno085::TareBasis::ROTATION_VECTOR);
    const auto z_only = parse_tare("4 3");
    if (!z_only.has_value()) {
        FAIL() << "tare refused the Z axis";
        return;
    }
    EXPECT_EQ(z_only->axes, Bno085::TareAxes::Z_ONLY);
    EXPECT_EQ(z_only->basis,
              Bno085::TareBasis::GYRO_INTEGRATED_ROTATION_VECTOR);
    EXPECT_FALSE(parse_tare("1 0"));
    EXPECT_FALSE(parse_tare("7 6"));
    EXPECT_FALSE(parse_tare("7"));
    EXPECT_FALSE(parse_tare("7 0 0"));
    EXPECT_FALSE(parse_tare(""));
}

TEST(BenchProtocol, CalibrationTakesThreeFlags) {
    using pico_bno085::bench::parse_calibration;
    const auto options = parse_calibration("1 0 1");
    if (!options.has_value()) {
        FAIL() << "cal refused three flags";
        return;
    }
    EXPECT_TRUE(options->accel);
    EXPECT_FALSE(options->gyro);
    EXPECT_TRUE(options->mag);

    // Each flag reaches its own field: a parser that read one flag twice
    // would fail one of these.
    struct Case {
        std::string_view text;
        bool accel;
        bool gyro;
        bool mag;
    };

    for (const Case& test_case :
         {Case{.text = "0 0 0", .accel = false, .gyro = false, .mag = false},
          Case{.text = "1 0 0", .accel = true, .gyro = false, .mag = false},
          Case{.text = "0 1 0", .accel = false, .gyro = true, .mag = false},
          Case{.text = "0 0 1", .accel = false, .gyro = false, .mag = true},
          Case{.text = "1 1 0", .accel = true, .gyro = true, .mag = false},
          Case{.text = "0 1 1", .accel = false, .gyro = true, .mag = true},
          Case{.text = "1 1 1", .accel = true, .gyro = true, .mag = true}}) {
        const auto parsed = parse_calibration(test_case.text);
        if (!parsed.has_value()) {
            FAIL() << test_case.text;
            return;
        }
        EXPECT_EQ(parsed->accel, test_case.accel) << test_case.text;
        EXPECT_EQ(parsed->gyro, test_case.gyro) << test_case.text;
        EXPECT_EQ(parsed->mag, test_case.mag) << test_case.text;
    }
    EXPECT_FALSE(parse_calibration("1 0"));
    EXPECT_FALSE(parse_calibration("1 0 2"));
    EXPECT_FALSE(parse_calibration("1 0 1 1"));
    EXPECT_FALSE(parse_calibration("101"));
}

TEST(BenchProtocol, EveryDriverErrorHasItsLp3Name) {
    using pico_bno085::Error;
    using pico_bno085::bench::error_name;
    EXPECT_EQ(error_name(Error::NONE), "none");
    EXPECT_EQ(error_name(Error::BUS_ERROR), "bus_error");
    EXPECT_EQ(error_name(Error::TIMEOUT), "timeout");
    EXPECT_EQ(error_name(Error::NOT_READY), "not_ready");
    EXPECT_EQ(error_name(Error::PROTOCOL), "protocol");
    EXPECT_EQ(error_name(Error::BUSY), "busy");
    EXPECT_EQ(error_name(Error::NO_ROOM), "no_room");
    EXPECT_EQ(error_name(Error::UNSUPPORTED), "unsupported");
    EXPECT_EQ(error_name(Error::INVALID_ARGUMENT), "invalid_argument");
}

TEST(BenchProtocol, PayloadIsOffAtPowerUp) {
    EXPECT_FALSE(pico_bno085::bench::PAYLOAD_DEFAULT);
}

TEST(BenchProtocol, SequenceNumbersAboveOneHundredTwentySevenPrintInFull) {
    SensorEvent event =
        make_event(SensorId::ACCELEROMETER, vec3(1.0F, 2.0F, 3.0F));
    event.sequence = 200;
    EXPECT_EQ(line_of(event, false), "E,1,200,1234567,3\n");
    EXPECT_EQ(line_of(event, true),
              "V,1,200,1234567,3,1.00000,2.00000,3.00000\n");
    event.sequence = 255;
    EXPECT_EQ(line_of(event, false), "E,1,255,1234567,3\n");
}

TEST(BenchProtocol, EveryWordSelectsItsCommand) {
    using pico_bno085::bench::split_command;
    using pico_bno085::bench::Word;
    EXPECT_EQ(split_command("bus spi").word, Word::BUS);
    EXPECT_EQ(split_command("bus spi").args, "spi");
    EXPECT_EQ(split_command("info").word, Word::INFO);
    EXPECT_EQ(split_command("payload 1").word, Word::PAYLOAD);
    EXPECT_EQ(split_command("cfg 5 2500").word, Word::CFG);
    EXPECT_EQ(split_command("off 5").word, Word::OFF);
    EXPECT_EQ(split_command("flush 5").word, Word::FLUSH);
    EXPECT_EQ(split_command("run 100").word, Word::RUN);
    EXPECT_EQ(split_command("stat").word, Word::STAT);
    EXPECT_EQ(split_command("heap").word, Word::HEAP);
    EXPECT_EQ(split_command("reset").word, Word::RESET);
    EXPECT_EQ(split_command("cleardcd").word, Word::CLEARDCD);
    EXPECT_EQ(split_command("trace 2").word, Word::TRACE);
    EXPECT_EQ(split_command("boot").word, Word::BOOT);
    EXPECT_EQ(split_command("tare 7 0").word, Word::TARE);
    EXPECT_EQ(split_command("tare 7 0").args, "7 0");
    EXPECT_EQ(split_command("tarepersist").word, Word::TAREPERSIST);
    EXPECT_EQ(split_command("cal 1 0 1").word, Word::CAL);
    EXPECT_EQ(split_command("calget").word, Word::CALGET);
    EXPECT_EQ(split_command("savedcd").word, Word::SAVEDCD);
    EXPECT_EQ(split_command("pdcd 1").word, Word::PDCD);
}

// A word that takes arguments is recognised without them, so that its parser
// answers `# err parse` (LP-2); one that takes none is unknown with any text.
TEST(BenchProtocol, AWordWithoutItsArgumentsIsRecognisedForParsing) {
    using pico_bno085::bench::split_command;
    using pico_bno085::bench::Word;
    for (const std::string_view bare :
         {"bus", "payload", "cfg", "off", "flush", "run", "trace", "tare",
          "cal", "pdcd"}) {
        EXPECT_NE(split_command(bare).word, Word::UNKNOWN) << bare;
        EXPECT_TRUE(split_command(bare).args.empty()) << bare;
    }
    for (const std::string_view extra :
         {"info x", "stat ", "heap 1", "reset 1", "cleardcd 1", "boot 1",
          "tarepersist 1", "calget 1", "savedcd 1", "nonsense", "tarex 7 0",
          ""}) {
        EXPECT_EQ(split_command(extra).word, Word::UNKNOWN) << extra;
    }
}

using pico_bno085::bench::RequestKind;
using pico_bno085::bench::RequestParse;

RequestParse request_of(std::string_view line) {
    return pico_bno085::bench::parse_calibration_request(
        pico_bno085::bench::split_command(line));
}

TEST(BenchProtocol, AMalformedRequestIsMalformedAndAnotherCommandIsNot) {
    using Status = RequestParse::Status;
    for (const std::string_view bare :
         {"tare", "tare 1 0", "cal", "cal 1 1", "pdcd", "pdcd 2", "pdcd 1 1"}) {
        EXPECT_EQ(request_of(bare).status, Status::MALFORMED) << bare;
    }
    for (const std::string_view other :
         {"stat", "cfg 5 2500", "payload 1", "bus", "bogus", ""}) {
        EXPECT_EQ(request_of(other).status, Status::NOT_A_REQUEST) << other;
    }
    for (const std::string_view fine : {"tare 7 0", "tarepersist", "cal 1 0 0",
                                        "calget", "savedcd", "pdcd 0"}) {
        EXPECT_EQ(request_of(fine).status, Status::OK) << fine;
    }
}

// Records the driver call a request makes, so the test sees which call and
// which bytes the firmware would send.
struct FakeImu {
    std::string call;
    Bno085::TareOptions tare;
    Bno085::CalibrationOptions calibration;
    std::uint8_t command = 0;
    std::vector<std::uint8_t> params;
    bool enable = false;
    pico_bno085::Result<void> result;

    pico_bno085::Result<void> tare_now(const Bno085::TareOptions& options) {
        call = "tare_now";
        tare = options;
        return result;
    }

    pico_bno085::Result<void> persist_tare() {
        call = "persist_tare";
        return result;
    }

    pico_bno085::Result<void> configure_dynamic_calibration(
        const Bno085::CalibrationOptions& options) {
        call = "configure_dynamic_calibration";
        calibration = options;
        return result;
    }

    pico_bno085::Result<void> send_command(
        std::uint8_t number, std::span<const std::uint8_t> bytes) {
        call = "send_command";
        command = number;
        params.assign(bytes.begin(), bytes.end());
        return result;
    }

    pico_bno085::Result<void> save_dcd() {
        call = "save_dcd";
        return result;
    }

    pico_bno085::Result<void> set_periodic_dcd(bool on) {
        call = "set_periodic_dcd";
        enable = on;
        return result;
    }
};

FakeImu send(std::string_view line) {
    FakeImu imu;
    const RequestParse parsed = request_of(line);
    EXPECT_EQ(parsed.status, RequestParse::Status::OK) << line;
    EXPECT_TRUE(pico_bno085::bench::send_request(imu, parsed.request).ok());
    return imu;
}

TEST(BenchProtocol, EachCommandMakesItsDriverCall) {
    EXPECT_EQ(send("tare 4 3").call, "tare_now");
    EXPECT_EQ(send("tare 4 3").tare.axes, Bno085::TareAxes::Z_ONLY);
    EXPECT_EQ(send("tare 4 3").tare.basis,
              Bno085::TareBasis::GYRO_INTEGRATED_ROTATION_VECTOR);
    EXPECT_EQ(send("tarepersist").call, "persist_tare");
    const FakeImu cal = send("cal 0 1 1");
    EXPECT_EQ(cal.call, "configure_dynamic_calibration");
    EXPECT_FALSE(cal.calibration.accel);
    EXPECT_TRUE(cal.calibration.gyro);
    EXPECT_TRUE(cal.calibration.mag);
    EXPECT_EQ(send("savedcd").call, "save_dcd");
}

// The flag reaches the driver as written; the driver maps it to the hub's
// inverted parameter (1000-3625 §6.4.7.1).
TEST(BenchProtocol, PeriodicDcdPassesItsFlagThrough) {
    const FakeImu on = send("pdcd 1");
    EXPECT_EQ(on.call, "set_periodic_dcd");
    EXPECT_TRUE(on.enable);
    EXPECT_FALSE(send("pdcd 0").enable);
}

TEST(BenchProtocol, CalibrationGetSendsSubcommandOneOfMeCalibration) {
    const FakeImu imu = send("calget");
    EXPECT_EQ(imu.call, "send_command");
    EXPECT_EQ(imu.command, pico_sh2::command::ME_CALIBRATION);
    EXPECT_EQ(imu.params, (std::vector<std::uint8_t>{0, 0, 0, 1}));
}

TEST(BenchProtocol, ARefusalReachesTheCaller) {
    FakeImu imu;
    imu.result = pico_bno085::Error::NOT_READY;
    const auto sent =
        pico_bno085::bench::send_request(imu, request_of("savedcd").request);
    EXPECT_EQ(sent.error(), pico_bno085::Error::NOT_READY);
}

TEST(BenchProtocol, EachRequestAwaitsTheResponseOfItsCommand) {
    using pico_bno085::bench::response_command;
    EXPECT_EQ(response_command(RequestKind::TARE), pico_sh2::command::TARE);
    EXPECT_EQ(response_command(RequestKind::TARE_PERSIST),
              pico_sh2::command::TARE);
    EXPECT_EQ(response_command(RequestKind::CALIBRATE),
              pico_sh2::command::ME_CALIBRATION);
    EXPECT_EQ(response_command(RequestKind::CALIBRATION_GET),
              pico_sh2::command::ME_CALIBRATION);
    EXPECT_EQ(response_command(RequestKind::SAVE_DCD),
              pico_sh2::command::SAVE_DCD);
    EXPECT_EQ(response_command(RequestKind::PERIODIC_DCD), 0);
}

CommandResponse response_to(std::uint8_t command) {
    CommandResponse response;
    response.command = command;
    return response;
}

TEST(BenchProtocol, NothingIsAwaitedAtPowerUp) {
    pico_bno085::bench::AwaitedResponse awaited;
    EXPECT_EQ(awaited.pending(), 0);
    EXPECT_FALSE(awaited.take(response_to(pico_sh2::command::INITIALIZE)));
    EXPECT_FALSE(awaited.take(response_to(0)));
}

TEST(BenchProtocol, OnlyTheAwaitedResponseIsTakenAndOnlyOnce) {
    pico_bno085::bench::AwaitedResponse awaited;
    awaited.expect(pico_sh2::command::SAVE_DCD);
    EXPECT_FALSE(awaited.take(response_to(pico_sh2::command::INITIALIZE)));
    EXPECT_FALSE(awaited.take(response_to(pico_sh2::command::TARE)));
    EXPECT_TRUE(awaited.take(response_to(pico_sh2::command::SAVE_DCD)));
    EXPECT_FALSE(awaited.take(response_to(pico_sh2::command::SAVE_DCD)));
}

TEST(BenchProtocol, ATimeoutEndsTheWait) {
    pico_bno085::bench::AwaitedResponse awaited;
    awaited.note_timeouts(3);
    awaited.expect(pico_sh2::command::TARE);
    awaited.note_timeouts(3);
    EXPECT_EQ(awaited.pending(), pico_sh2::command::TARE);
    awaited.note_timeouts(4);
    EXPECT_EQ(awaited.pending(), 0);
    EXPECT_FALSE(awaited.take(response_to(pico_sh2::command::TARE)));
}

TEST(BenchProtocol, ARefusalEndsTheWaitExceptBusy) {
    using pico_bno085::Error;
    pico_bno085::bench::AwaitedResponse awaited;
    awaited.expect(pico_sh2::command::TARE);
    awaited.refused(Error::BUSY);
    EXPECT_EQ(awaited.pending(), pico_sh2::command::TARE);
    awaited.refused(Error::NOT_READY);
    EXPECT_EQ(awaited.pending(), 0);
}

TEST(BenchProtocol, AHubResetEndsTheWait) {
    pico_bno085::bench::AwaitedResponse awaited;
    awaited.note_hub_resets(2);
    awaited.expect(pico_sh2::command::TARE);
    awaited.note_hub_resets(2);
    EXPECT_EQ(awaited.pending(), pico_sh2::command::TARE);
    awaited.note_hub_resets(3);
    EXPECT_EQ(awaited.pending(), 0);
}

TEST(BenchProtocol, AnAcceptedRestartEndsTheWaitAndARefusedOneFollowsTheRule) {
    using pico_bno085::Error;
    pico_bno085::bench::AwaitedResponse awaited;
    awaited.expect(pico_sh2::command::TARE);
    awaited.restart(pico_bno085::Result<void>(Error::BUSY));
    EXPECT_EQ(awaited.pending(), pico_sh2::command::TARE);
    awaited.restart(pico_bno085::Result<void>());
    EXPECT_EQ(awaited.pending(), 0);
    awaited.expect(pico_sh2::command::TARE);
    awaited.restart(pico_bno085::Result<void>(Error::NOT_READY));
    EXPECT_EQ(awaited.pending(), 0);
}

TEST(BenchProtocol, ACommandWithNoResponseEndsAnEarlierWait) {
    pico_bno085::bench::AwaitedResponse awaited;
    awaited.expect(pico_sh2::command::TARE);
    awaited.expect(
        pico_bno085::bench::response_command(RequestKind::PERIODIC_DCD));
    EXPECT_EQ(awaited.pending(), 0);
    EXPECT_FALSE(awaited.take(response_to(pico_sh2::command::TARE)));
}

TEST(BenchProtocol, ABusArgumentIsMissingUnknownOrOneOfTwo) {
    using pico_bno085::bench::BusChoice;
    using pico_bno085::bench::parse_bus;
    EXPECT_EQ(parse_bus(""), BusChoice::MISSING);
    EXPECT_EQ(parse_bus("  "), BusChoice::MISSING);
    EXPECT_EQ(parse_bus("i2c"), BusChoice::I2C);
    EXPECT_EQ(parse_bus("spi"), BusChoice::SPI);
    for (const std::string_view other : {"x", "I2C", "i2c x", " spi", "uart"}) {
        EXPECT_EQ(parse_bus(other), BusChoice::UNKNOWN) << other;
    }
}

TEST(BenchProtocol, AClearEndsTheWait) {
    pico_bno085::bench::AwaitedResponse awaited;
    awaited.expect(pico_sh2::command::ME_CALIBRATION);
    awaited.clear();
    EXPECT_FALSE(awaited.take(response_to(pico_sh2::command::ME_CALIBRATION)));
}

// A row of the bandwidth table in LP-8: the longest line a sensor prints, its
// header at its widest and each field at the widest value its Q point allows.
struct Row {
    SensorId sensor;
    EventPayload payload;
    std::size_t rate_hz;
    std::size_t line_bytes;
};

std::size_t widest_line(const Row& row) {
    SensorEvent event = make_event(row.sensor, row.payload);
    event.sequence = std::numeric_limits<std::uint8_t>::max();
    event.event_us = std::numeric_limits<std::uint64_t>::max();
    return line_of(event, true).size();
}

TEST(BenchProtocol, WorstEnabledSetFitsHalfTheFullSpeedBulkCeiling) {
    // Rates are the datasheet's Figure 6-16 maxima. The stabilised rotation
    // vectors take their non-stabilised versions' rates (1000-3927 §6.9); the
    // uncalibrated gyroscope and magnetometer, which the figure does not list,
    // take the calibrated sensors' rates (UNCONFIRMED). Quaternion parts reach
    // -2 (Q14), accuracy -8 (Q12), accelerations -128 (Q8), gyroscope -64 (Q9),
    // magnetic field -2048 (Q4) and angular velocity -32 (Q10).
    constexpr float QUAT = -2.0F;
    constexpr float ACC = -8.0F;
    const Quaternion quat{.i = QUAT, .j = QUAT, .k = QUAT, .real = QUAT};
    const RotationSample rotation{.unit = quat, .accuracy_rad = ACC};
    const Vec3 accel_min = vec3(-128, -128, -128);
    const std::array<Row, 13> rows{{
        {.sensor = SensorId::GYRO_INTEGRATED_ROTATION_VECTOR,
         .payload =
             GyroIntegratedSample{.unit = quat,
                                  .angular_velocity = vec3(-32, -32, -32)},
         .rate_hz = 1000,
         .line_bytes = 98},
        {.sensor = SensorId::ROTATION_VECTOR,
         .payload = rotation,
         .rate_hz = 400,
         .line_bytes = 76},
        {.sensor = SensorId::GAME_ROTATION_VECTOR,
         .payload = quat,
         .rate_hz = 400,
         .line_bytes = 67},
        {.sensor = SensorId::GEOMAGNETIC_ROTATION_VECTOR,
         .payload = rotation,
         .rate_hz = 90,
         .line_bytes = 76},
        {.sensor = SensorId::GRAVITY,
         .payload = accel_min,
         .rate_hz = 400,
         .line_bytes = 64},
        {.sensor = SensorId::LINEAR_ACCELERATION,
         .payload = accel_min,
         .rate_hz = 400,
         .line_bytes = 64},
        {.sensor = SensorId::ACCELEROMETER,
         .payload = accel_min,
         .rate_hz = 500,
         .line_bytes = 64},
        {.sensor = SensorId::GYROSCOPE,
         .payload = vec3(-64, -64, -64),
         .rate_hz = 400,
         .line_bytes = 61},
        {.sensor = SensorId::MAGNETIC_FIELD,
         .payload = vec3(-2048, -2048, -2048),
         .rate_hz = 100,
         .line_bytes = 67},
        {.sensor = SensorId::ARVR_STABILIZED_ROTATION_VECTOR,
         .payload = rotation,
         .rate_hz = 400,
         .line_bytes = 77},
        {.sensor = SensorId::ARVR_STABILIZED_GAME_ROTATION_VECTOR,
         .payload = quat,
         .rate_hz = 400,
         .line_bytes = 68},
        {.sensor = SensorId::GYROSCOPE_UNCALIBRATED,
         .payload = UncalibratedSample{.value = vec3(-64, -64, -64),
                                       .bias = vec3(-64, -64, -64)},
         .rate_hz = 400,
         .line_bytes = 91},
        {.sensor = SensorId::MAGNETIC_FIELD_UNCALIBRATED,
         .payload = UncalibratedSample{.value = vec3(-2048, -2048, -2048),
                                       .bias = vec3(-2048, -2048, -2048)},
         .rate_hz = 100,
         .line_bytes = 104},
    }};
    std::size_t total = 0;
    for (const Row& row : rows) {
        EXPECT_EQ(widest_line(row), row.line_bytes);
        total += row.line_bytes * row.rate_hz;
    }
    constexpr std::size_t WORST_BYTES_PER_S = 381'140;
    EXPECT_EQ(total, WORST_BYTES_PER_S);
    // USB 2.0 full-speed bulk: nineteen 64-byte packets in each 1 ms frame.
    constexpr std::size_t CEILING_BYTES_PER_S = std::size_t{19} * 64 * 1000;
    EXPECT_LE(total, CEILING_BYTES_PER_S / 2);
}

}  // namespace
