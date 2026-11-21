# pico-bno085 Architecture

The [specification](technical-specification.md) defines guarantees. The
[usage guide](usage-guide.md) covers wiring and startup.

## Scope

[pico-sh2](https://github.com/jamesfxley/pico-sh2) implements SHTP framing,
the SH-2 session and report decoding. This library adds RP2350 transports,
reset and strap sequencing, interrupts and device policy.

`pico_bno085::core` is the aggregate target. `pico_bno085::driver` exposes the
portable façade. The HAL and testing targets are described below. Internal
strict targets in the export set are implementation details.

The seeded channel map permits a reset before the first advertisement,
including recovery from a session stuck before READY. An advertisement
replaces the seed if the hub announces another base.

```
        application
             │
      ┌──────▼───────┐
      │    Bno085    │  driver/  — desired-sensor table, re-apply after reset,
      │   (façade)   │             calibration forwarders, stats, logging
      └──────┬───────┘
             │ owns a
      ┌──────▼───────┐
      │   Session    │  pico-sh2
      └──────┬───────┘
             │ talks through
      ┌──────▼────────────────────┐
      │      IHalTransport        │
      │  I2cHalTransport / SpiHalTransport  hal/rp2350/
      │            │                        (firmware only)
      │      TransportCore ── ControlPins, HintnLatch, the pin set
      └───────────────────────────┘
                   │
        split_read.hpp, edge_latch.hpp,             hal/
        detail/strap_plan.hpp, detail/pin_mux.hpp
        (no SDK — host-tested)

        tracing_transport.hpp                      testing
        (installed; see below)
```

## HAL modules

The HAL separates portable logic from SDK calls so protocol handling can be
tested without flashing a board. Each module has its own directory and target
(CXX-13.1).

`pico_bno085::hal_core` is header-only and builds on host and firmware:

- `SplitReadCompensator` removes the continuation flag and extra sequence
  advance caused by an I2C header peek. A genuine unfinished cargo retains its
  continuation flag. Without compensation, the framing engine would see false
  fragments and sequence gaps (HR-6).
- `EdgeLatch` passes HINTN timestamps from interrupt to `service()` context.
  A 64-bit timestamp spans two 32-bit stores on the M33, so
  `pico_util::Published` uses `SEQLOCK_WRITER_NOT_PREEMPTIBLE`. The interrupt
  writer completes before the mainline reader resumes, so the reader's wait
  terminates. The consumable flag and fallback count belong to the latch (AR-2).
- `detail::strap_plan` orders NRST, PS1, PS0, BOOTN and I2C-only SA0 writes.
  `ControlPins::init()` executes the host-tested plan (HR-3).
- `detail::pin_mux` checks the GPIO bank and peripheral mapping against the
  RP2350 datasheet, Table 677. Transport `init()` checks custom pins before
  touching them. `pins.hpp` checks default pins at compile time.

`pico_bno085::hal` contains firmware-only I2C, SPI, GPIO and timer adapters
under `src/hal/rp2350/`. Public headers retain the `pico_bno085/hal/` prefix
(CXX-13.3).

`TransportCore` shares control pins, the HINTN latch, clock and counters between
buses. `TransportCounters` has the same meaning for either transport.
`HintnLatch` uses raw GPIO handlers so an application's default GPIO callback
remains available. The dispatch table retains its registered mask because SDK
removal requires that mask and duplicate pin registration asserts.

## The testing target

`pico_bno085::testing` exports the header-only `TracingTransport` decorator for
host and firmware. Tests, the benchmark and downstream consumers use it to
observe transfers through `IHalTransport`. Its caller supplies the output sink.
Printing in the sink adds service time before the next transfer.

## The two transports

| Operation | I2C | SPI |
|---|---|---|
| Read | Peek at the header, then read the cargo with compensation | Read header and body within one CSN assertion |
| Absent hub | Address not acknowledged | Reserved dead-bus header |
| Write | Write directly | Assert WAKE, wait for HINTN, assert CSN, release WAKE (HR-1) |
| Empty read | Pass through a null header | Pass through a null header |

Both transports report read absence successfully so the session can recover
from a reset. A read error would fault startup or restart the READY handshake.
BNO086 empty-poll behaviour remains unvalidated (FW-2).

Retrieval reads the 4-byte header for the length, then rereads the header and
cargo (1000-3927 §1.3.1); the specification gives the reason (HR-6). HINTN
asserts per transfer, including each part of a split read (1000-3535 §2.6).
After the second transfer, the latch discards the answered assertion so it
cannot back a later cargo's read or latency period (HR-5, HR-6).
A coalesced assertion for another cargo still reaches the live-level fallback
(HR-1). SPI writes use the same discard when no duplex cargo was retained.

Service latency starts at the first unserviced edge, since a retry would
otherwise hide a delayed read. Fallback reads have no fresh edge to measure.
The benchmark clears the peak before each run because reset and boot waits
can create startup outliers (HR-5).

## Policy the façade owns

`Session` implements the protocol without retaining consumer intent. `Bno085`
keeps a desired-sensor table and reapplies it after startup or reset. The table
can exceed the transmit queue's depth, so per-entry pending flags let a pass
resume over several `service()` calls. Flags remain attached to entries when
sensors are enabled or disabled between calls.

An outstanding reset triggers reapplication even if the entire handshake
completes within one `service()` call and no not-ready state is observed.
`enable_sensor()` reports `APPLIED` or `DEFERRED`, allowing a caller to tell
whether its recorded configuration has been sent. A full queue schedules
reapplication. Reissuing Set Feature is idempotent.

Typed tare axes and basis prevent exchanging the two arguments. Named
calibration fields prevent exchanging positional flags. `TareAxes` exposes
only the two supported bitmaps (1000-3625 §6.4.3.1).

## Façade boundaries

`Stats` contains driver counters. Session counters remain accessible through
`counters()` and transport counters through the supplied transport. Copying
those sets into `Stats` would make dependency counter additions break this API.
Reset notifications identify no cause, so `hub_resets` counts requested and
spontaneous resets. Correlating requests with notifications could hide a
spontaneous reset between the request and its transmission.

The façade, `HintnLatch` and `TransportCore` cannot be copied or moved. Session
sinks retain the façade's address, and interrupt dispatch retains the latch's.

## Allocation

No driver allocation occurs after `begin()`. The desired table and the session
use the same profile capacity, so the driver cannot desire more sensors than
the session holds. `pico_util::HeapWatermark` measures growth from a baseline
armed after session construction. The `b-heap` case checks that growth.
Example drivers use static storage to keep fixed session buffers off the
4 KiB core-0 stack and include them in static footprint measurements.

## Startup timing and build checks

Version assertions run in the driver translation unit because the umbrella
header has no compiled target. Private CMake definitions bind those assertions
to the build version. The assertions also run in firmware builds and in
consumer builds that compile the library from source, where the host version
test does not run.

The 30 ms I2C transaction timeout covers a full large-profile transfer at the
minimum baud rate with clock-stretch margin. It bounds a bus fault or wedged
clock stretch; it is not the HINTN service-latency budget.

The 2 ms reset hold allows a host that cannot time a shorter pulse precisely.
The boot delay and phase deadlines are timeout guards: the datasheet gives
`t1` as at least 90 ms without a maximum (1000-3927 §6.5.3). SPI's WAKE timeout
also has margin above the typical 150 microseconds, for which no maximum is
given (1000-3927 Figure 6-12).

## Custom wiring

Consumers set control lines in `ControlConfig` and bus pins and the peripheral
instance in the transport's `Config`. The
[usage guide](usage-guide.md#pins-the-bno085-multiplexes) describes the pin-mux
rules.
`init()` returns `INVALID_ARGUMENT` for unreachable or out-of-bank bus pins
and an out-of-bank HINTN pin.

## Bus selection

The benchmark releases the outgoing transport's interrupt and pads before
initialising the other transport. Initialisation drives straps with NRST
asserted, then the session releases reset. The hub samples the straps at reset.
Switching therefore restarts the hub and loses its session state. This suits
a benchmark comparing buses. An application should keep one bus selected.
