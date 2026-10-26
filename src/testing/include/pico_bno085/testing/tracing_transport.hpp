// SPDX-License-Identifier: MIT
#pragma once

/// \file tracing_transport.hpp
/// A transport decorator that reports the raw bytes of every transfer.
///
/// The session talks to the hub through one seam, so that seam is the only
/// place the whole conversation is visible. When a hub misbehaves — a cargo
/// that will not parse, a handshake that stalls — the wire is the evidence, and
/// without this the only way to see it is to edit a transport and reflash.
///
/// The bytes go to a sink rather than to stdout: library code has no business
/// choosing an output, and a caller that formats them has to accept that
/// printing inside service() costs time the next deadline may need.
///
/// Free of the Pico SDK: it decorates the interface, not a bus.

#include <cstdint>
#include <span>

#include "pico_sh2/sh2/hal_transport.hpp"
#include "pico_sh2/util/result.hpp"
#include "pico_util/result/callback.hpp"

namespace pico_bno085::testing {

/// Which way a traced transfer moved.
enum class TraceDirection : std::uint8_t {
    READ,   ///< Bytes retrieved from the hub.
    WRITE,  ///< Bytes sent to the hub.
};

/// One traced transfer.
struct TraceRecord {
    /// Which way the transfer moved.
    TraceDirection direction = TraceDirection::READ;
    /// The bytes moved. Empty when \ref error is set: a read that failed moved
    /// nothing the caller should read.
    ///
    /// **Borrowed for the duration of the sink call.** The span aliases the
    /// buffer the transfer used — the session's staging buffer on a read, the
    /// caller's frame on a write — and neither survives the call. This struct
    /// is a copyable aggregate, so copying the record by value copies a span
    /// that will dangle; a sink that keeps anything must copy the *bytes*.
    std::span<const std::uint8_t> bytes;
    /// The transfer's error, or NONE.
    pico_sh2::Sh2Error error = pico_sh2::Sh2Error::NONE;
    /// Successful read's transport HINTN edge, in microseconds.
    /// Zero on writes and errors; copied from ReceivedTransfer::intn_edge_us.
    std::uint64_t intn_edge_us = 0;
};

/// Where traced transfers are reported. Runs inside service(), and the record
/// it is handed does not outlive the call — see TraceRecord::bytes.
using TraceSink = pico_util::Callback<void(const TraceRecord&)>;

/// Wraps another transport and reports every transfer to a sink.
class TracingTransport final : public pico_sh2::IHalTransport {
   public:
    /// \param inner the transport to decorate. It must outlive this object.
    explicit TracingTransport(pico_sh2::IHalTransport& inner)
        : inner_(&inner) {}

    /// Set where traced transfers are reported.
    void set_sink(TraceSink sink) { sink_ = sink; }

    /// Point the decorator at a different transport. For a harness that picks
    /// its bus at run time and so cannot bind one at construction.
    ///
    /// \param inner the transport to decorate from now on.
    void set_inner(pico_sh2::IHalTransport& inner) { inner_ = &inner; }

    /// Turn tracing on or off. It is off until asked for: the sink runs inside
    /// service(), between a transfer and the next one the hub is waiting to be
    /// read.
    ///
    /// \param on whether to report transfers.
    void set_tracing(bool on) { tracing_ = on; }

    /// Whether tracing is on.
    [[nodiscard]] bool tracing() const { return tracing_; }

    /// Forwards to the decorated transport.
    bool data_ready() override { return inner_->data_ready(); }

    /// Reads through the decorated transport, then reports the bytes it moved.
    ///
    /// \param dst staging span for the transfer.
    /// \return the decorated transport's result, unchanged.
    pico_sh2::Result<pico_sh2::ReceivedTransfer> read_transfer(
        std::span<std::uint8_t> dst) override {
        auto result = inner_->read_transfer(dst);
        if (tracing_ && sink_) {
            TraceRecord record;
            record.direction = TraceDirection::READ;
            if (result.ok()) {
                record.bytes = dst.first(result.value().length);
                record.intn_edge_us = result.value().intn_edge_us;
            } else {
                record.error = result.error();
            }
            sink_(record);
        }
        return result;
    }

    /// Writes through the decorated transport, then reports the frame.
    ///
    /// \param frame the bytes to send.
    /// \return the decorated transport's result, unchanged.
    pico_sh2::Result<void> write_transfer(
        std::span<const std::uint8_t> frame) override {
        auto result = inner_->write_transfer(frame);
        if (tracing_ && sink_) {
            TraceRecord record;
            record.direction = TraceDirection::WRITE;
            record.bytes = frame;
            record.error =
                result.ok() ? pico_sh2::Sh2Error::NONE : result.error();
            sink_(record);
        }
        return result;
    }

    /// Forwards to the decorated transport.
    std::uint64_t now_us() override { return inner_->now_us(); }

    /// Forwards to the decorated transport.
    void set_reset(bool asserted) override { inner_->set_reset(asserted); }

   private:
    pico_sh2::IHalTransport* inner_;
    TraceSink sink_;
    bool tracing_ = false;
};

}  // namespace pico_bno085::testing
