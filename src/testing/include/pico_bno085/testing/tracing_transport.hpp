// SPDX-License-Identifier: MIT
#pragma once

/// \file tracing_transport.hpp
/// Transfer tracing through a caller-supplied sink. Printing inside service()
/// can delay the next transfer.

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
    /// Transfer bytes. A read error carries nothing a caller should read.
    /// A write error retains its frame. The span is borrowed during the sink
    /// call. A sink retaining data must copy the bytes.
    std::span<const std::uint8_t> bytes;
    /// The transfer's error, or NONE.
    pico_sh2::Sh2Error error = pico_sh2::Sh2Error::NONE;
    /// Successful read's HINTN edge in microseconds. Zero on writes and errors.
    std::uint64_t intn_edge_us = 0;
};

/// Transfer sink, called inside service(). Records are borrowed for the call.
using TraceSink = pico_util::Callback<void(const TraceRecord&)>;

/// Wraps another transport and reports every transfer to a sink.
class TracingTransport final : public pico_sh2::IHalTransport {
   public:
    /// \param inner the transport to decorate. It must outlive this object.
    explicit TracingTransport(pico_sh2::IHalTransport& inner)
        : inner_(&inner) {}

    /// Set where traced transfers are reported.
    void set_sink(TraceSink sink) { sink_ = sink; }

    /// Select a different transport.
    void set_inner(pico_sh2::IHalTransport& inner) { inner_ = &inner; }

    /// Enable or disable tracing. Initially off.
    void set_tracing(bool on) { tracing_ = on; }

    /// Whether tracing is on.
    [[nodiscard]] bool tracing() const { return tracing_; }

    /// Forwards to the decorated transport.
    bool data_ready() override { return inner_->data_ready(); }

    /// Read and trace through the inner transport.
    /// \return the inner transport's result unchanged.
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

    /// Write and trace through the inner transport.
    /// \return the inner transport's result unchanged.
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
