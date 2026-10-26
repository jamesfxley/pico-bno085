// SPDX-License-Identifier: MIT
// OR-1: the wire is visible at the seam the session talks through, without a
// transport edit and a reflash.
#include "pico_bno085/testing/tracing_transport.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "pico_sh2/sh2/hal_transport.hpp"
#include "pico_sh2/util/result.hpp"

namespace {

using pico_bno085::testing::TraceDirection;
using pico_bno085::testing::TraceRecord;
using pico_bno085::testing::TraceSink;
using pico_bno085::testing::TracingTransport;

// A transport that answers from a script, so the decorator is what is under
// test rather than a bus.
// final: the base declares its destructor protected and non-virtual on purpose
// (a transport is owned by whoever supplies it, never deleted through this
// pointer). A derived class with virtual functions and a public non-virtual
// destructor reopens exactly that hazard unless it cannot be derived from.
class FakeTransport final : public pico_sh2::IHalTransport {
   public:
    std::vector<std::uint8_t> next_read;
    pico_sh2::Sh2Error read_error = pico_sh2::Sh2Error::NONE;
    pico_sh2::Sh2Error write_error = pico_sh2::Sh2Error::NONE;
    std::vector<std::uint8_t> last_write;
    bool reset_state = false;

    bool data_ready() override { return true; }

    std::uint64_t now_us() override { return 99; }

    void set_reset(bool asserted) override { reset_state = asserted; }

    pico_sh2::Result<pico_sh2::ReceivedTransfer> read_transfer(
        std::span<std::uint8_t> dst) override {
        if (read_error != pico_sh2::Sh2Error::NONE) {
            return read_error;
        }
        // A real transport reads into the staging span and reports what fit,
        // so its length is <= dst.size() by construction. The fake is the only
        // thing that can violate that, and the decorator takes the length
        // straight to dst.first(n) — where a length past the end is undefined
        // rather than a test failure. Clamping keeps the fake to the contract
        // it stands in for (EN-04.1).
        const std::size_t served = std::min(next_read.size(), dst.size());
        std::ranges::copy(std::span(next_read).first(served), dst.begin());
        return pico_sh2::ReceivedTransfer{.length = served, .intn_edge_us = 42};
    }

    pico_sh2::Result<void> write_transfer(
        std::span<const std::uint8_t> frame) override {
        last_write.assign(frame.begin(), frame.end());
        if (write_error != pico_sh2::Sh2Error::NONE) {
            return write_error;
        }
        return {};
    }
};

struct Capture {
    std::vector<TraceDirection> directions;
    std::vector<std::vector<std::uint8_t>> payloads;
    std::vector<pico_sh2::Sh2Error> errors;
    std::vector<std::uint64_t> edges;

    void on_trace(const TraceRecord& record) {
        directions.push_back(record.direction);
        payloads.emplace_back(record.bytes.begin(), record.bytes.end());
        errors.push_back(record.error);
        edges.push_back(record.intn_edge_us);
    }

    TraceSink sink() {
        return TraceSink::bind<Capture, &Capture::on_trace>(this);
    }
};

TEST(TracingTransport, PassesEverythingThroughUntraced) {
    FakeTransport fake;
    fake.next_read = {1, 2, 3, 4};
    TracingTransport tracer(fake);

    std::array<std::uint8_t, 8> buf{};
    EXPECT_TRUE(tracer.data_ready());
    EXPECT_EQ(tracer.now_us(), 99U);
    tracer.set_reset(true);
    EXPECT_TRUE(fake.reset_state);

    auto record = tracer.read_transfer(buf);
    ASSERT_TRUE(record.ok());
    EXPECT_EQ(record.value().length, 4U);
    EXPECT_EQ(record.value().intn_edge_us, 42U);
}

TEST(TracingTransport, ReportsNothingUntilTracingIsOn) {
    FakeTransport fake;
    fake.next_read = {1, 2, 3, 4};
    Capture cap;
    TracingTransport tracer(fake);
    tracer.set_sink(cap.sink());

    std::array<std::uint8_t, 8> buf{};
    (void)tracer.read_transfer(buf);
    EXPECT_TRUE(cap.directions.empty());

    tracer.set_tracing(true);
    (void)tracer.read_transfer(buf);
    EXPECT_EQ(cap.directions.size(), 1U);
}

// OR-1: a wire trace preserves the anchor needed to reconstruct event times.
TEST(TracingTransport, CopiesTheReadEdgeAndZerosWritesAndErrors) {
    FakeTransport fake;
    fake.next_read = {0x04, 0x00, 0x02, 0x07};
    Capture cap;
    TracingTransport tracer(fake);
    tracer.set_sink(cap.sink());
    tracer.set_tracing(true);
    std::array<std::uint8_t, 8> buf{};
    ASSERT_TRUE(tracer.read_transfer(buf).ok());
    ASSERT_TRUE(tracer.write_transfer(fake.next_read).ok());
    fake.read_error = pico_sh2::Sh2Error::TIMEOUT;
    EXPECT_FALSE(tracer.read_transfer(buf).ok());
    fake.write_error = pico_sh2::Sh2Error::BUS_ERROR;
    EXPECT_FALSE(tracer.write_transfer(fake.next_read).ok());
    ASSERT_EQ(cap.edges.size(), 4U);
    EXPECT_EQ(cap.edges[0], 42U);
    EXPECT_EQ(cap.edges[1], 0U);
    EXPECT_EQ(cap.edges[2], 0U);
    EXPECT_EQ(cap.edges[3], 0U);
}

TEST(TracingTransport, ReportsTheBytesAReadActuallyMoved) {
    FakeTransport fake;
    fake.next_read = {0x04, 0x00, 0x02, 0x07};
    Capture cap;
    TracingTransport tracer(fake);
    tracer.set_sink(cap.sink());
    tracer.set_tracing(true);

    std::array<std::uint8_t, 32> buf{};
    (void)tracer.read_transfer(buf);

    ASSERT_EQ(cap.payloads.size(), 1U);
    EXPECT_EQ(cap.directions[0], TraceDirection::READ);
    // The span is the transfer, not the whole staging buffer.
    EXPECT_EQ(cap.payloads[0], fake.next_read);
    EXPECT_EQ(cap.errors[0], pico_sh2::Sh2Error::NONE);
}

TEST(TracingTransport, ReportsAFailedReadAsAnErrorWithNoBytes) {
    FakeTransport fake;
    fake.read_error = pico_sh2::Sh2Error::TIMEOUT;
    Capture cap;
    TracingTransport tracer(fake);
    tracer.set_sink(cap.sink());
    tracer.set_tracing(true);

    std::array<std::uint8_t, 8> buf{};
    auto record = tracer.read_transfer(buf);
    EXPECT_FALSE(record.ok());

    ASSERT_EQ(cap.errors.size(), 1U);
    EXPECT_EQ(cap.errors[0], pico_sh2::Sh2Error::TIMEOUT);
    // Nothing was moved, so there is nothing to show.
    EXPECT_TRUE(cap.payloads[0].empty());
}

TEST(TracingTransport, ReportsWritesAndTheirOutcome) {
    FakeTransport fake;
    Capture cap;
    TracingTransport tracer(fake);
    tracer.set_sink(cap.sink());
    tracer.set_tracing(true);

    std::vector<std::uint8_t> frame = {0x05, 0x00, 0x02, 0x00, 0xFD};
    ASSERT_TRUE(tracer.write_transfer(frame).ok());
    ASSERT_EQ(cap.directions.size(), 1U);
    EXPECT_EQ(cap.directions[0], TraceDirection::WRITE);
    EXPECT_EQ(cap.payloads[0], frame);
    EXPECT_EQ(cap.errors[0], pico_sh2::Sh2Error::NONE);

    fake.write_error = pico_sh2::Sh2Error::BUS_ERROR;
    EXPECT_FALSE(tracer.write_transfer(frame).ok());
    EXPECT_EQ(cap.errors[1], pico_sh2::Sh2Error::BUS_ERROR);
    // The frame still reaches the transport: tracing observes, it does not
    // gate.
    EXPECT_EQ(fake.last_write, frame);
}

TEST(TracingTransport, ToleratesTracingWithNoSink) {
    FakeTransport fake;
    fake.next_read = {1, 2, 3, 4};
    TracingTransport tracer(fake);
    tracer.set_tracing(true);

    std::array<std::uint8_t, 8> buf{};
    EXPECT_TRUE(tracer.read_transfer(buf).ok());
}

}  // namespace
