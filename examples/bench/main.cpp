// SPDX-License-Identifier: MIT
// Device benchmark. The line protocol is specified in
// docs/technical-specification.md (LP); the checklist is in README.md.
// Heap growth is measured after session construction (AR-1).
#include <hardware/timer.h>
#include <pico/error.h>
#include <pico/stdio.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "bench_protocol.hpp"
#include "pico_bno085/driver/bno085.hpp"
#include "pico_bno085/hal/i2c_hal_transport.hpp"
#include "pico_bno085/hal/spi_hal_transport.hpp"
#include "pico_bno085/hal/transport_core.hpp"
#include "pico_bno085/testing/tracing_transport.hpp"
#include "pico_sh2/reports/decoders.hpp"
#include "pico_sh2/reports/sensor_event.hpp"
#include "pico_sh2/reports/sensor_id.hpp"
#include "pico_sh2/sh2/command.hpp"
#include "pico_sh2/sh2/hal_transport.hpp"
#include "pico_sh2/sh2/session.hpp"
#include "pico_sh2/util/result.hpp"
#include "pico_util/heap/heap_watermark.hpp"

namespace {

using pico_bno085::bench::BASE_DEC;
using pico_bno085::bench::BASE_HEX;
using pico_bno085::bench::LINE_BUFFER_SIZE;
using pico_bno085::bench::take_uint;
using pico_sh2::reports::SensorEvent;
using pico_sh2::reports::SensorId;

constexpr std::uint64_t US_PER_MS = 1'000;
constexpr std::size_t LINE_CAP = 64;

// Timestamp tick in microseconds (1000-3625 §7.2.1) (PR-3).
constexpr unsigned TICK_US = 100;

// Mode 1 caps serial output. Mode 2 emits complete reads for timing diagnosis.
unsigned g_trace_mode = 0;

void print_trace(void* /*ctx*/,
                 const pico_bno085::testing::TraceRecord& record) {
    if (g_trace_mode == 2 &&
        record.direction == pico_bno085::testing::TraceDirection::READ) {
        if (record.error != pico_sh2::Sh2Error::NONE) {
            printf("# wire edge_us=0 err=%u\n",
                   static_cast<unsigned>(record.error));
            return;
        }
        // Fixed storage avoids a temporary payload.
        // Direct stdio avoids a temporary string (OR-1).
        printf("# wire edge_us=%llu n=%zu:",
               static_cast<unsigned long long>(record.intn_edge_us),
               record.bytes.size());
        for (const auto byte : record.bytes) {
            printf(" %02x", static_cast<unsigned>(byte));
        }
        printf("\n");
        return;
    }
    constexpr std::size_t MAX_DUMP = 32;
    const char* tag =
        record.direction == pico_bno085::testing::TraceDirection::READ ? "rx"
                                                                       : "tx";
    if (record.error != pico_sh2::Sh2Error::NONE) {
        printf("# %s err=%u\n", tag, static_cast<unsigned>(record.error));
        return;
    }
    printf("# %s n=%zu:", tag, record.bytes.size());
    const std::size_t shown =
        record.bytes.size() < MAX_DUMP ? record.bytes.size() : MAX_DUMP;
    for (std::size_t i = 0; i < shown; ++i) {
        printf(" %02x", static_cast<unsigned>(record.bytes[i]));
    }
    printf("%s\n", shown < record.bytes.size() ? " ..." : "");
}

// Static transports allow bus selection without allocation (AR-1).
pico_bno085::hal::I2cHalTransport g_i2c{{}};
pico_bno085::hal::SpiHalTransport g_spi{{}};

pico_sh2::IHalTransport* g_selected = nullptr;
pico_bno085::testing::TracingTransport* g_tracer = nullptr;
const char* g_bus_name = "none";

// Streaming state for the active `run`, updated by the event sink.
struct Run {
    bool streaming = false;
    // Payload lines replace E lines for kinds that have one (LP-5).
    bool payload = pico_bno085::bench::PAYLOAD_DEFAULT;
    std::uint32_t events = 0;
};

Run g_run;

// Post-init heap growth (AR-1), armed once the session is constructed.
pico_util::HeapWatermark g_heap;

// Static storage keeps session buffers off the 4 KiB core-0 stack and
// includes them in footprint measurements.
pico_bno085::testing::TracingTransport g_tracer_object(g_i2c);
pico_bno085::Bno085 g_imu(g_tracer_object);

void on_event(void* ctx, const SensorEvent& event) {
    Run* run = static_cast<Run*>(ctx);
    if (!run->streaming) {
        return;
    }
    ++run->events;
    // One line per event: E, or its payload line when payload is on (LP-4,
    // LP-5). Formatting into a buffer keeps the bytes those of the host test.
    std::array<char, LINE_BUFFER_SIZE> line{};
    if (pico_bno085::bench::format_event(line, event, run->payload) > 0) {
        printf("%s", line.data());
    }
}

// The response the host is waiting for (LP-7).
pico_bno085::bench::AwaitedResponse g_awaited;

void on_command_response(void* /*ctx*/,
                         const pico_sh2::CommandResponse& response) {
    if (!g_awaited.take(response)) {
        return;
    }
    std::array<char, LINE_BUFFER_SIZE> line{};
    if (pico_bno085::bench::format_command_response(line, response) > 0) {
        printf("%s", line.data());
    }
}

// Clear the latency peak on the selected transport before each run.
void clear_service_latency_peak() {
    if (g_selected == &g_spi) {
        g_spi.clear_service_latency_peak();
    } else if (g_selected == &g_i2c) {
        g_i2c.clear_service_latency_peak();
    }
}

// Read counters from the selected transport.
pico_bno085::hal::TransportCounters transport_counters() {
    if (g_selected == &g_spi) {
        return g_spi.counters();
    }
    if (g_selected == &g_i2c) {
        return g_i2c.counters();
    }
    return {};
}

// Walk session counters so new counters reach the host with their own labels.
void print_stats(const pico_bno085::Bno085& imu) {
    const pico_bno085::Stats stats = imu.stats();
    printf("# stats bus=%s ready=%d state=%u fault=%u fault_phase=%u",
           g_bus_name, imu.ready() ? 1 : 0, static_cast<unsigned>(imu.state()),
           static_cast<unsigned>(imu.fault()),
           static_cast<unsigned>(imu.startup_phase()));

    pico_sh2::for_each_counter(
        imu.counters(),
        [](const char* name, const char* /*description*/, std::uint32_t value) {
            printf(" %s=%lu", name, static_cast<unsigned long>(value));
        });

    const auto counters = transport_counters();
    printf(
        " hub_resets=%lu recoveries=%lu reenabled=%lu rejections=%lu "
        "enabled=%u diag_events=%lu level_fallbacks=%lu absent_reads=%lu "
        "service_latency_max_us=%lu\n",
        static_cast<unsigned long>(stats.hub_resets),
        static_cast<unsigned long>(stats.recoveries),
        static_cast<unsigned long>(stats.sensors_reenabled),
        static_cast<unsigned long>(stats.config_rejections),
        static_cast<unsigned>(stats.enabled_sensors),
        static_cast<unsigned long>(stats.diag_events),
        static_cast<unsigned long>(counters.level_fallback_reads),
        static_cast<unsigned long>(counters.absent_reads),
        static_cast<unsigned long>(counters.service_latency_max_us));
}

// Read a command without blocking service(). Return an empty view until
// complete.
std::string_view poll_line(std::array<char, LINE_CAP>& buf, std::size_t& len) {
    int ch = getchar_timeout_us(0);
    while (ch != PICO_ERROR_TIMEOUT) {
        if (ch == '\n' || ch == '\r') {
            if (len > 0) {
                const std::string_view line(buf.data(), len);
                len = 0;
                return line;
            }
        } else if (len < buf.size()) {
            buf[len++] = static_cast<char>(ch);
        }
        ch = getchar_timeout_us(0);
    }
    return {};
}

// Initialise only the selected transport to avoid pad contention.
void select_bus(pico_bno085::Bno085& imu, std::string_view which) {
    // Release the outgoing transport's interrupt and pads before re-strapping
    // with NRST asserted (HR-3).
    if (g_selected == &g_i2c) {
        g_i2c.end();
    } else if (g_selected == &g_spi) {
        g_spi.end();
    }
    g_selected = nullptr;
    g_bus_name = "none";

    pico_sh2::Result<void> started = pico_sh2::Sh2Error::INVALID_ARGUMENT;
    if (which == "i2c") {
        started = g_i2c.init();
        g_selected = &g_i2c;
        g_bus_name = "i2c";
    } else if (which == "spi") {
        started = g_spi.init();
        g_selected = &g_spi;
        g_bus_name = "spi";
    } else {
        printf("# err bus must be i2c or spi\n");
        return;
    }

    if (!started.ok()) {
        printf("# err transport init\n");
        g_selected = nullptr;
        g_bus_name = "none";
        return;
    }

    g_tracer->set_inner(*g_selected);
    imu.begin();
    printf("# bus %s\n", g_bus_name);
}

void print_info() {
    printf("# info tick_us=%u max_enabled=%u bus=%s\n", TICK_US,
           static_cast<unsigned>(pico_bno085::MAX_ENABLED_SENSORS), g_bus_name);
}

// Each command group reports whether it recognised the line.

void report(bool ok, const char* done) { printf("# %s\n", ok ? done : "err"); }

// cfg: a sensor id and an interval.
void handle_cfg(pico_bno085::Bno085& imu, std::string_view args) {
    unsigned id = 0;
    unsigned interval = 0;
    if (!take_uint(args, id, BASE_HEX) ||
        !take_uint(args, interval, BASE_DEC)) {
        printf("# err parse\n");
        return;
    }
    report(imu.enable_sensor(static_cast<SensorId>(id),
                             {.report_interval_us = interval})
               .ok(),
           "ok");
}

// off/flush: a sensor id. Disable removes desired sensors that would
// otherwise survive reset (AR-3).
void handle_off_or_flush(pico_bno085::Bno085& imu, bool is_off,
                         std::string_view args) {
    unsigned id = 0;
    if (!take_uint(args, id, BASE_HEX)) {
        printf("# err parse\n");
        return;
    }
    const auto sensor = static_cast<SensorId>(id);
    report(is_off ? imu.disable_sensor(sensor).ok() : imu.flush(sensor).ok(),
           "ok");
}

// run: a duration in milliseconds.
void handle_run(std::string_view args, std::uint64_t& run_deadline_us) {
    unsigned ms = 0;
    if (!take_uint(args, ms, BASE_DEC)) {
        printf("# err parse\n");
        return;
    }
    g_run.events = 0;
    g_run.streaming = true;
    clear_service_latency_peak();
    run_deadline_us = time_us_64() + static_cast<std::uint64_t>(ms) * US_PER_MS;
    printf("# run %u\n", ms);
}

// trace: a mode.
void handle_trace(std::string_view args) {
    unsigned on = 0;
    if (!take_uint(args, on, BASE_DEC)) {
        printf("# err parse\n");
        return;
    }
    g_trace_mode = on;
    g_tracer->set_tracing(on != 0);
    printf("# trace %u\n", on);
}

// payload: 0 or 1. It needs no bus (LP-2).
void handle_payload(std::string_view args) {
    const auto payload = pico_bno085::bench::parse_flag(args);
    if (!payload) {
        printf("# err parse\n");
        return;
    }
    g_run.payload = *payload;
    printf("# payload %u\n", *payload ? 1U : 0U);
}

// stat/heap: report only.
void handle_stat(pico_bno085::Bno085& imu) { print_stats(imu); }

void handle_heap() {
    // Heap growth must not occur after startup (AR-1).
    printf("# heap armed=%d growth=%lu\n", g_heap.armed() ? 1 : 0,
           static_cast<unsigned long>(g_heap.growth_bytes()));
}

// reset/cleardcd/boot restart the hub, so a command in flight gets no
// response (LP-7).
void handle_restart(pico_bno085::Bno085& imu, pico_bno085::bench::Word word) {
    using pico_bno085::bench::Word;
    g_awaited.clear();
    if (word == Word::RESET) {
        // Calibration survival across executable-channel reset is UNCONFIRMED
        // (FW-2).
        report(imu.reset().ok(), "reset");
    } else if (word == Word::CLEARDCD) {
        report(imu.clear_dcd_and_reset().ok(), "cleardcd");
    } else {
        // begin() can retry startup from FAULTED without reflashing.
        imu.begin();
        printf("# boot\n");
    }
}

// Reply with the driver's refusal named (LP-3).
void print_refusal(pico_bno085::Error error) {
    const std::string_view name = pico_bno085::bench::error_name(error);
    printf("# err %.*s\n", static_cast<int>(name.size()), name.data());
}

// tare/tarepersist/cal/calget/savedcd/pdcd (LP-2, LP-3, LP-7). Returns false
// for a command that is none of them.
bool handle_calibration_command(pico_bno085::Bno085& imu,
                                const pico_bno085::bench::Command& command) {
    using pico_bno085::bench::RequestParse;
    const RequestParse parsed =
        pico_bno085::bench::parse_calibration_request(command);
    if (parsed.status == RequestParse::Status::NOT_A_REQUEST) {
        return false;
    }
    if (parsed.status == RequestParse::Status::MALFORMED) {
        printf("# err parse\n");
        return true;
    }
    const auto sent = pico_bno085::bench::send_request(imu, parsed.request);
    if (!sent.ok()) {
        g_awaited.refused(sent.error());
        print_refusal(sent.error());
        return true;
    }
    // An accepted command makes its response the next one printed. The hub
    // sends none to pdcd (1000-3625 §6.4.7), which waits for none.
    g_awaited.expect(pico_bno085::bench::response_command(parsed.request.kind));
    printf("# ok\n");
    return true;
}

void handle_command(pico_bno085::Bno085& imu, std::string_view line,
                    std::uint64_t& run_deadline_us) {
    using pico_bno085::bench::Word;
    const pico_bno085::bench::Command command =
        pico_bno085::bench::split_command(line);
    if (command.word == Word::BUS) {
        select_bus(imu, command.args);
        return;
    }
    if (command.word == Word::INFO) {
        print_info();
        return;
    }
    if (command.word == Word::PAYLOAD) {
        handle_payload(command.args);
        return;
    }

    // Commands below require an initialised hub.
    if (g_selected == nullptr) {
        printf("# err no bus selected\n");
        return;
    }

    switch (command.word) {
        case Word::CFG:
            handle_cfg(imu, command.args);
            return;
        case Word::OFF:
        case Word::FLUSH:
            handle_off_or_flush(imu, command.word == Word::OFF, command.args);
            return;
        case Word::RUN:
            handle_run(command.args, run_deadline_us);
            return;
        case Word::TRACE:
            handle_trace(command.args);
            return;
        case Word::STAT:
            handle_stat(imu);
            return;
        case Word::HEAP:
            handle_heap();
            return;
        case Word::RESET:
        case Word::CLEARDCD:
        case Word::BOOT:
            handle_restart(imu, command.word);
            return;
        case Word::UNKNOWN:
        case Word::BUS:
        case Word::INFO:
        case Word::PAYLOAD:
        case Word::TARE:
        case Word::TAREPERSIST:
        case Word::CAL:
        case Word::CALGET:
        case Word::SAVEDCD:
        case Word::PDCD:
            break;
    }
    if (handle_calibration_command(imu, command)) {
        return;
    }
    // Silence would cost the harness a read timeout per typo.
    printf("# err unknown command\n");
}

}  // namespace

int main() {
    stdio_init_all();

    // No transport is called before bus selects one.
    g_tracer = &g_tracer_object;
    g_tracer->set_sink(pico_bno085::testing::TraceSink(&print_trace, nullptr));

    pico_bno085::Bno085& imu = g_imu;
    imu.set_event_sink(pico_sh2::reports::EventSink(&on_event, &g_run));
    imu.set_command_sink(
        pico_sh2::Session::CommandSink(&on_command_response, nullptr));

    // Arm after session construction to measure steady-state growth (AR-1).
    g_heap.arm();

    printf("# ready-for-bus\n");
    print_info();

    bool was_ready = false;
    std::uint64_t run_deadline_us = 0;
    std::array<char, LINE_CAP> line_buf{};
    std::size_t len = 0;

    while (true) {
        if (g_selected != nullptr) {
            imu.service();
            // A command the hub never answers times out in the session, which
            // ends the wait for its response (LP-7).
            g_awaited.note_timeouts(imu.counters().command_timeouts);

            // Announce transitions to READY for startup and recovery timing.
            const bool ready = imu.ready();
            if (ready && !was_ready) {
                printf("# ready\n");
            }
            was_ready = ready;

            if (g_run.streaming && time_us_64() >= run_deadline_us) {
                g_run.streaming = false;
                print_stats(imu);
                printf("# done events=%lu\n",
                       static_cast<unsigned long>(g_run.events));
            }
        }

        const std::string_view line = poll_line(line_buf, len);
        if (!line.empty()) {
            handle_command(imu, line, run_deadline_us);
        }
    }
}
