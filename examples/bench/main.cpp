// SPDX-License-Identifier: MIT
// On-device validation harness for the BNO085 driver.
//
// Brings the hub up over either bus and exposes a USB CDC line protocol that
// bench.py drives to run the device checklist. The firmware only measures and
// reports; every pass/fail threshold lives in the harness, so the device stays
// a dumb instrument and a threshold change needs no reflash.
//
// The bus is chosen at run time. Both transports are compiled in and only the
// selected one is initialised, so a single image covers both halves of the
// checklist and b-parity can compare them without a reflash in between.
//
// Pin-electrical diagnostics are outside this harness. Use trace to inspect
// transfers and boot to retry startup; the usage guide describes the sensor's
// power, straps and reference wiring.
//
// Protocol, host -> device (one command per line):
//   bus i2c|spi          select the bus and bring the hub up, re-strapping
//   cfg <id_hex> <us>    enable/reconfigure a sensor at a report interval
//   off <id_hex>         disable a sensor
//   run <ms>             stream events for a while, then a '# stats' summary
//   stat                 emit a '# stats' line now
//   reset                restart the hub, using the executable-channel reset
//   cleardcd             clear the calibration and reset
//   flush <id_hex>       force a batching sensor to deliver now
//   heap                 report post-init heap growth (AR-1)
//   trace 0|1|2          off, capped transfers, or full reads with edge
//   boot                 re-run the handshake without reflashing
//   info                 constants the harness needs to interpret the output
#include <hardware/timer.h>
#include <pico/error.h>
#include <pico/stdio.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <system_error>

#include "pico_bno085/driver/bno085.hpp"
#include "pico_bno085/hal/i2c_hal_transport.hpp"
#include "pico_bno085/hal/spi_hal_transport.hpp"
#include "pico_bno085/hal/transport_core.hpp"
#include "pico_bno085/testing/tracing_transport.hpp"
#include "pico_sh2/reports/decoders.hpp"
#include "pico_sh2/reports/sensor_event.hpp"
#include "pico_sh2/reports/sensor_id.hpp"
#include "pico_sh2/sh2/hal_transport.hpp"
#include "pico_sh2/sh2/session.hpp"
#include "pico_sh2/util/result.hpp"
#include "pico_util/heap/heap_watermark.hpp"

namespace {

using pico_sh2::reports::SensorEvent;
using pico_sh2::reports::SensorId;

constexpr std::uint64_t US_PER_MS = 1'000;
constexpr std::size_t LINE_CAP = 64;
constexpr int BASE_DEC = 10;
constexpr int BASE_HEX = 16;

// Resolution of the base-delta and report-delay fields (1000-3625 §7.2.1).
// PR-3 follows 1000-3927 §1.3.5.3 Figure 1-36 for the delay sign. The
// harness needs it to bound timestamp ordering, and reads it from `info` rather
// than restating it.
constexpr unsigned TICK_US = 100;

// --- wire tracing ------------------------------------------------------------

// Mode 1 caps output to limit serial service time. Mode 2 deliberately emits
// complete reads for the wire-timing diagnostic.
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
        // OR-1: fixed storage and direct stdio, no temporary payload or string.
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

// --- the session under test --------------------------------------------------

// Both transports exist for the whole run; `bus` decides which is brought up.
// Static storage, so selecting a bus allocates nothing (AR-1).
pico_bno085::hal::I2cHalTransport g_i2c{{}};
pico_bno085::hal::SpiHalTransport g_spi{{}};

pico_sh2::IHalTransport* g_selected = nullptr;
pico_bno085::testing::TracingTransport* g_tracer = nullptr;
const char* g_bus_name = "none";

// Streaming state for the active `run`, updated by the event sink.
struct Run {
    bool streaming = false;
    std::uint32_t events = 0;
};

Run g_run;

// Post-init heap growth (AR-1), armed once the session is constructed.
pico_util::HeapWatermark g_heap;

// Static, not local to main(). Bno085 embeds the session's fixed-capacity
// buffers, so it is 2.2 KiB under the default profile and 3.8 KiB under the
// large one, against a 4 KiB core-0 stack — on the stack the large profile
// leaves a few hundred bytes for everything main() calls, printf included.
// Static storage also puts it where the footprint gate can see it.
pico_bno085::testing::TracingTransport g_tracer_object(g_i2c);
pico_bno085::Bno085 g_imu(g_tracer_object);

void on_event(void* ctx, const SensorEvent& event) {
    Run* run = static_cast<Run*>(ctx);
    if (!run->streaming) {
        return;
    }
    ++run->events;
    // One line per event: sensor, per-sensor sequence, reconstructed timestamp,
    // accuracy. bench.py derives rate, drop count (sequence gaps), and jitter.
    printf("E,%u,%u,%llu,%u\n", static_cast<unsigned>(event.sensor),
           static_cast<unsigned>(event.sequence),
           static_cast<unsigned long long>(event.event_us),
           static_cast<unsigned>(event.accuracy));
}

// Start a fresh service-latency window on the live transport, so a run's figure
// describes the run and not the startup outlier (see
// TransportCore::clear_service_latency_peak).
void clear_service_latency_peak() {
    if (g_selected == &g_spi) {
        g_spi.clear_service_latency_peak();
    } else if (g_selected == &g_i2c) {
        g_i2c.clear_service_latency_peak();
    }
}

// The transport counters, whichever bus is live. Both answer the same
// questions, so the harness never needs to know which it is reading.
pico_bno085::hal::TransportCounters transport_counters() {
    if (g_selected == &g_spi) {
        return g_spi.counters();
    }
    if (g_selected == &g_i2c) {
        return g_i2c.counters();
    }
    return {};
}

// The stats line is `key=value` pairs, which the harness parses generically.
// The session's counters are emitted by walking the set rather than naming each
// one, so adding a counter in pico-sh2 reaches the bench without an edit here —
// and no counter can be printed under another's label.
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

// --- line protocol -----------------------------------------------------------

// The argument text of a command whose prefix has already matched. Taking the
// offset from the prefix itself is what keeps the two from disagreeing: a
// hand-written length is right only until the command is renamed.
constexpr std::string_view arguments(std::string_view line,
                                     std::string_view prefix) {
    return line.substr(prefix.size());
}

// Parse a whitespace-delimited unsigned field at the given base out of `text`,
// advancing `text` past it. Returns false if no digits parse.
bool take_uint(std::string_view& text, unsigned& out, int base) {
    while (!text.empty() && text.front() == ' ') {
        text.remove_prefix(1);
    }
    auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), out, base);
    if (ec != std::errc()) {
        return false;
    }
    text.remove_prefix(static_cast<std::size_t>(ptr - text.data()));
    return true;
}

// Read one newline-terminated command into `buf` without blocking the service
// loop. Returns the line as a view when complete, empty otherwise.
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

// Bring up one bus. Only the selected transport is initialised, so the other's
// pads are never claimed and the two cannot contend.
void select_bus(pico_bno085::Bno085& imu, std::string_view which) {
    // Switching buses is a re-strap, which is legitimate because init() drives
    // the straps with the hub held in reset (HR-3) and then the session
    // releases it. Release the outgoing transport's interrupt first so its
    // latch stops claiming a dispatch slot and its pads are no longer serviced.
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

// Each family below reports whether it recognised the line, so handle_command
// stays a dispatch and the parse-then-act shape is written once per family
// rather than once per command.

void report(bool ok, const char* done) { printf("# %s\n", ok ? done : "err"); }

// cfg/off/flush: a sensor id, and for cfg an interval.
bool handle_sensor_command(pico_bno085::Bno085& imu, std::string_view line) {
    if (line.starts_with("cfg ")) {
        std::string_view args = arguments(line, "cfg ");
        unsigned id = 0;
        unsigned interval = 0;
        if (!take_uint(args, id, BASE_HEX) ||
            !take_uint(args, interval, BASE_DEC)) {
            printf("# err parse\n");
            return true;
        }
        report(imu.enable_sensor(static_cast<SensorId>(id),
                                 {.report_interval_us = interval})
                   .ok(),
               "ok");
        return true;
    }
    // Sensors accumulate across cfg commands, and the desired table survives a
    // reset by design (AR-3), so a run can only isolate one sensor if the
    // harness can also take one away.
    if (line.starts_with("off ") || line.starts_with("flush ")) {
        const bool is_off = line.starts_with("off ");
        std::string_view args =
            is_off ? arguments(line, "off ") : arguments(line, "flush ");
        unsigned id = 0;
        if (!take_uint(args, id, BASE_HEX)) {
            printf("# err parse\n");
            return true;
        }
        const auto sensor = static_cast<SensorId>(id);
        report(
            is_off ? imu.disable_sensor(sensor).ok() : imu.flush(sensor).ok(),
            "ok");
        return true;
    }
    return false;
}

// run/trace: a single decimal argument.
bool handle_timed_command(std::string_view line,
                          std::uint64_t& run_deadline_us) {
    if (line.starts_with("run ")) {
        std::string_view args = arguments(line, "run ");
        unsigned ms = 0;
        if (!take_uint(args, ms, BASE_DEC)) {
            printf("# err parse\n");
            return true;
        }
        g_run.events = 0;
        g_run.streaming = true;
        clear_service_latency_peak();
        run_deadline_us =
            time_us_64() + static_cast<std::uint64_t>(ms) * US_PER_MS;
        printf("# run %u\n", ms);
        return true;
    }
    if (line.starts_with("trace ")) {
        std::string_view args = arguments(line, "trace ");
        unsigned on = 0;
        if (!take_uint(args, on, BASE_DEC)) {
            printf("# err parse\n");
            return true;
        }
        g_trace_mode = on;
        g_tracer->set_tracing(on != 0);
        printf("# trace %u\n", on);
        return true;
    }
    return false;
}

// stat/heap/reset/cleardcd/boot: no arguments.
bool handle_bare_command(pico_bno085::Bno085& imu, std::string_view line) {
    if (line == "stat") {
        print_stats(imu);
    } else if (line == "heap") {
        // AR-1: nothing on the steady-state path allocates, so this must not
        // move once the session is up.
        printf("# heap armed=%d growth=%lu\n", g_heap.armed() ? 1 : 0,
               static_cast<unsigned long>(g_heap.growth_bytes()));
    } else if (line == "reset") {
        // Restart via the executable channel; calibration survival is
        // UNCONFIRMED. This is what a boot or recovery measurement wants.
        report(imu.reset().ok(), "reset");
    } else if (line == "cleardcd") {
        report(imu.clear_dcd_and_reset().ok(), "cleardcd");
    } else if (line == "boot") {
        // begin() restarts the handshake from any state, FAULTED included, so a
        // wiring fix can be retried without reflashing.
        imu.begin();
        printf("# boot\n");
    } else {
        return false;
    }
    return true;
}

void handle_command(pico_bno085::Bno085& imu, std::string_view line,
                    std::uint64_t& run_deadline_us) {
    if (line.starts_with("bus ")) {
        select_bus(imu, arguments(line, "bus "));
        return;
    }
    if (line == "info") {
        print_info();
        return;
    }

    // Everything below needs a hub. An error beats a plausible-looking
    // measurement of nothing.
    if (g_selected == nullptr) {
        printf("# err no bus selected\n");
        return;
    }

    if (handle_sensor_command(imu, line) ||
        handle_timed_command(line, run_deadline_us) ||
        handle_bare_command(imu, line)) {
        return;
    }
    // Silence would cost the harness a read timeout per typo.
    printf("# err unknown command\n");
}

}  // namespace

int main() {
    stdio_init_all();

    // The tracer is pointed at the selected transport by `bus`; until then it
    // stands in front of one that is never called, because no command before
    // `bus` reaches it.
    g_tracer = &g_tracer_object;
    g_tracer->set_sink(pico_bno085::testing::TraceSink(&print_trace, nullptr));

    pico_bno085::Bno085& imu = g_imu;
    imu.set_event_sink(pico_sh2::reports::EventSink(&on_event, &g_run));

    // Everything the session will ever allocate is allocated by construction,
    // so growth measured from here is growth on the steady-state path (AR-1).
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

            // Announce every rising edge to READY, so bench.py can time startup
            // and observe recovery after a reset.
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
