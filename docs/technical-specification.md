# pico-bno085 Technical Specification

**Status:** Normative.

This specification defines the architecture, hardware-quirk, performance,
boundary-safety, timing, and observability requirements for the pico-bno085
BNO085 driver on the Raspberry Pi Pico 2 (RP2350). Every requirement has an ID
(`AR`/`HR`/`PR`/`BR`/`TR`/`OR`) and is mapped to an acceptance test in the
traceability matrix. The platform-independent SH-2/SHTP protocol layer is
specified separately in [pico-sh2](https://github.com/jamesfxley/pico-sh2); this
document covers the device driver and its RP2350 transports. Measured device
results live in [examples/bench/baselines/](../examples/bench/baselines/).

---

## 1. Scope and targets

| | |
|---|---|
| Hardware target | An RP2350 board. This repository builds for `pico2_w`, and the bench runs on a Pico 2 W; as a dependency the consumer's RP2350 board is taken as given, and running on a plain Pico 2 is UNCONFIRMED. The BNO085 is wired per [usage-guide.md](usage-guide.md). BNO086 is supported by the same code (its zero-length-packet polling behaviour is tolerated). |
| Transports | I2C (≤400 kHz) and SPI (mode 3, ≤3 MHz), selected in firmware via the PS0/PS1 pins driven before reset. |
| Protocol | SH-2 over SHTP, via the `pico-sh2` dependency. |
| Out of scope | UART-RVC / UART-SHTP modes; DFU / bootloader firmware update; the SH-2 protocol logic itself (specified in pico-sh2). |

Sizing follows the `PICO_BNO085_PROFILE` option (`default` / `large`), which
forwards to `PICO_SH2_PROFILE`.

**API stability:** the public API (`namespace pico_bno085`, headers under
`pico_bno085/`) follows semantic versioning; breaking changes only at a major
version.

---

## 2. Architecture requirements (AR)

- **AR-1 — Zero post-init heap on device.** After `Bno085::begin()` completes,
  the driver performs no dynamic allocation. Verified on device by the bench
  suite's `HeapWatermark` growth counter reading zero (`b-heap`). Separately,
  both example images stay within <= 20 KiB of static RAM and <= 128 KiB of
  flash (all `LOAD` sections), which the footprint gate checks on every
  firmware build; it bounds the static footprint and cannot see an allocation.
  Both halves, because the two move independently: a fixed-capacity buffer
  added to the session lands in RAM, a report decoder added to the table lands
  in flash.
- **AR-2 — The interrupt handler stores the edge and returns.** For each
  registered pin it identifies and acknowledges its edge, reads the timer and
  the live level, and, while the line is still asserted (HR-1), latches a flag
  and a microsecond timestamp through `EdgeLatch::latch`. It performs no bus
  I/O and allocates nothing. The work is bounded by construction rather than
  by a timing budget: a fixed loop over the dispatch table.

  The 64-bit timestamp is published under a seqlock, so a `service()`-context
  reader never observes a value torn across the two 32-bit stores the M33
  emits. The fences are full thread fences, not signal fences, so the
  publication stays correct if the reader ever moves to the second core.

  A read is anchored to the edge read in the same step that clears the flag, and
  the transport returns it with the transfer. `data_ready()` can see HINTN
  asserted before the handler has run, so an anchor taken before the read would
  be the previous edge's, on a read counted as edged. An edge latched before the
  read clears the flag is that read's anchor; one latched after leaves the read
  a counted level fallback (HR-1).

  Implemented in `edge_latch` and `hintn_irq`; the anchor is returned with each
  read by `transport_core` and the I2C and SPI transports. The latency this bounds in
  practice is HR-5's, measured; the handler's own duration is not instrumented.
- **AR-3 — Desired-sensor re-enable across reset.** The driver records the
  desired configuration of each enabled sensor and, when the session re-reaches
  READY after a spontaneous hub reset (pico-sh2 SR-5), re-applies every desired
  configuration without consumer intervention. The desired table describes
  intent, not session state, so it survives the reset. Host-tested against the
  `FakeHub` reset path (`bno085_test`).
- **AR-4 — The driver owns the error taxonomy it publishes.** Every fallible
  entry point on the façade returns `pico_bno085::Result<T>`, which carries
  `pico_bno085::Error`. pico-sh2's `Sh2Error` is mapped at the boundary by
  `from_sh2()`: it collapses the distinctions a driver consumer has no separate
  response to: the four SHTP and session framing codes onto `PROTOCOL`, both
  capacity codes onto `NO_ROOM`. No `pico_sh2` type appears in a fallible
  signature, so a consumer's exhaustive switch cannot be broken by a release of
  a dependency. An enumerator appended by a later pico-sh2 is a compile error
  here rather than a silent remapping: the mapping switch names every code and
  carries no `default:`, so `-Wswitch` under `-Werror` fails the build until
  this driver decides where the new code belongs. The transports are
  outside this: they implement `pico_sh2::IHalTransport` and answer in its
  taxonomy, which is that seam's contract rather than a re-export. Host-tested
  by `bno085_test`.
- **AR-5 — A board without an RP2350 is refused at configure.** pico-logging's
  sink and level atomics need the RP2350's exclusive access, so the driver
  declares `PLATFORM rp2350` to pico-tooling's bootstrap, which refuses a board
  without what a library declares. A consumer adding it on an RP2040 board fails
  at configure, naming the platform.

## 3. Hardware requirements (HR)

- **HR-1 — Never blind-poll.** A bus `read_transfer` begins only while HINTN is
  asserted (a stashed duplex cargo needs no bus activity): never on a blind
  schedule, and never on a latched edge alone. The
  hub asserts HINT when it has a cargo to be read and deasserts it after the
  read begins (1000-3535 §2.6), so the live level says a cargo is waiting and
  the latched falling edge says when (AR-2); a missed or coalesced edge cannot
  wedge `service()`, since the read does not wait on one. The level is only as
  current as the hub's deassert: §2.6 lets it lag "including after the
  transaction is complete", and on the BNO08x it falls typically 10 µs after
  an I2C address is recognised and 800 ns after CSN (1000-3927 Figure 6-12,
  tclid and tcsid). A pass reaching `data_ready()` inside that lag reads an
  answered assertion: a null header on SPI, and on a BNO085 over I2C a clock
  stretch until the next cargo or the transfer timeout (HR-2). The level is
  also asserted to answer an SPI WAKE, with either cargo or a null header
  (1000-3535
  §3.3); the write performs that transfer. An I2C split read's continuation
  follows its header without re-checking the level: 1000-3535 §2.5 says
  "Reading cargoes may only begin after the Sensor Hub asserts the host
  interrupt signal", and whether a continuation issued before the hub
  reasserts complies is UNCONFIRMED; a BNO085 not ready for it stretches the
  clock, which HR-2's timeout bounds. An edge can outlive
  its assertion: a handler held off past the read that answered it runs after
  it. The handler therefore drops an edge whose line has already risen, and a
  transport drops an assertion that a transfer it performed answered: a split
  read's continuation (HR-6) and a null-header SPI wake reply. A duplex
  cargo instead retains the answering edge for the stashed read. An edge whose
  handler runs after the read begins but before the hub deasserts, typically
  10 µs on I2C and 800 ns on SPI (1000-3927 Figure 6-12, tclid and tcsid), is
  still latched; it causes no read, but the next read counts as edged on it,
  and, since a later edge does not move a span already open, that read's
  service latency is measured from it (HR-5), early by up to one report
  interval. A BNO085
  polled with no data available stretches the I2C clock indefinitely and a
  BNO086 returns zero-length packets (1000-3927 §1.2.2.2: a polled BNO085 with
  nothing to send "will stretch the clock until it has data", while a BNO086
  "will send zero data packets" the host is told to read the length of and
  ignore); the driver relies on neither and tolerates both (an empty or dead-bus read is handed to the engine, never faulted), and
  the session bounds the transfers drained per `service()` so a stuck-low line
  cannot spin it. A read with no latched edge behind it, a level fallback, is
  anchored (AR-2) to the edge latched when it clears the flag: an
  earlier edge, unless the handler latched the fresh one in between, and the
  events it carries are timed from that. The HAL counts these reads
  (`level_fallback_reads`), an upper bound on the fallback reads anchored early, so a
  device run can bound how often it occurs and attribute any PR-3 jitter to it. Implemented in the HAL transports.
- **HR-2 — Timeout-bounded I2C, and an unacknowledged read is absence, not a
  fault.** Every I2C transaction uses the SDK's `*_timeout_us` variants; a
  stretch or bus fault surfaces `TIMEOUT`/`BUS_ERROR` and never hangs
  `service()`. The two SDK failure returns carry different meanings and are not
  conflated: `PICO_ERROR_TIMEOUT` is a stretched or wedged bus and is a fault,
  while `PICO_ERROR_GENERIC` is "address not acknowledged, no device present"
  and means the hub has left the bus. Because a read is only issued behind a
  HINTN assertion (HR-1), a hub that does not acknowledge one has left since
  asserting, which is a reset in progress. The transport reports it as the
  zero-length transfer a hub that cannot answer produces — the only thing a
  transport with no acknowledge bit can report — and counts it in
  `absent_reads`; recovery is the session's (pico-sh2 SR-5). The split-read
  state the transport carries across transfers (continuation and per-channel
  peek-sequence compensation, HR-6) is cleared with it, since the hub abandons
  any part-sent cargo and restarts its channel sequence numbers while away.
  Implemented in `i2c_hal_transport`.
- **HR-3 — Reset sequencing.** The straps the selected bus requires are driven
  before NRST is released; the session holds NRST low for its `reset_hold_us`
  (2 ms, against a `tnrst` minimum of 10 ns in 1000-3927 §6.5.3 —
  six orders of magnitude of margin, chosen for the hub's boot rather than for
  the pin) and bounds the boot and advertisement
  phases by its own deadlines. PS0/PS1/BOOTN are driven in both modes; SA0 is an
  I2C-only strap because the hub multiplexes it with SPI data-in (SA0/H_MOSI),
  so in SPI mode the pin is left to the SPI transport and no strap driver can
  contend with it on a shared net. Implemented in `control_pins`, which drives
  the steps `detail::strap_plan` orders.
- **HR-4 — SPI handshake.** SPI runs mode 3 at ≤ 3 MHz; a host-initiated write
  performs the WAKE → HINTN → CSN handshake with a bounded deadline; PS0 is
  repurposed as WAKE only after reset. Neither transport runs its bus above
  the hub's limit: `init()` refuses a requested clock above 3 MHz for SPI
  (1000-3927 §6.5.2) or 400 kHz for I2C (§6.5.1) with `INVALID_ARGUMENT`
  before any pin is touched, and refuses the same way, releasing the
  peripheral, when the rate the SDK actually sets is above it. Implemented in `spi_hal_transport` and `i2c_hal_transport`.
  A write receives the coincident hub cargo or null header in the same duplex
  transfer, padding the shorter direction (1000-3535 §3.3, §2.3.1). The next
  read returns that stashed cargo with the answering edge.

- **HR-5 — HINTN service latency.** With the documented main loop, the gap
  between HINTN asserting and the read answering it stays within 1 ms and an
  order of magnitude below the hub's retry threshold. 1000-3927 §1.2.4.1 gives
  that threshold: a host failing to answer an H_INTN assertion within
  approximately 10 ms causes the hub to time out, deassert and retry, and the
  datasheet's own guidance is to handle H_INTN within a tenth of the fastest
  sensor period. The transports
  report it as `service_latency_max_us`, a high-water mark over a measurement
  window the caller starts, and `b-lat` gates on it.

  The gap is measured from the edge that opened the unserviced span, not the
  most recent one. The hub deasserts and retries an assertion left unserviced,
  so a pass that missed its budget produces a fresh edge; measuring from that
  would report microseconds for exactly the run the figure exists to catch.
  `EdgeLatch` holds the span start alongside the newest edge for this reason.

  The window matters too: startup spends the reset hold and boot delay in timed
  waits by design, so an edge arriving mid-wait is serviced late for a reason
  that says nothing about the steady-state loop. A cumulative mark would report
  that startup figure forever.

  The anchor is a separate question from the measurement. A read served by the
  level fallback may anchor its events to an earlier edge (HR-1). A read that answers a retried assertion anchors to the retry's edge;
  whether the hub re-takes the cargo's base delta against the retry is
  UNCONFIRMED (1000-3927 §1.2.4.1 says only that it deasserts and retries).
- **HR-6 — I2C split reads are invisible above the HAL.** The I2C transport
  retrieves a cargo by reading the 4-byte header to learn its length and then
  reading the cargo itself, the split read SHTP defines for a host that does not
  know the length in advance (1000-3535 §2.3.1). The hub answers such a read by
  re-offering the cargo with the length field's continuation bit set, and it
  increments the channel's sequence number once per read, so a cargo retrieved
  this way consumes two. Both are artifacts of the transport's own read
  strategy, not properties of the cargo: the transport clears the continuation
  bit it caused and subtracts the sequence numbers its peeks consumed, so the
  engine sees one unfragmented cargo per sequence number and its drop detection
  stays exact. A continuation bit that follows a cargo genuinely truncated by
  the staging span is the hub's own and is passed through. Implemented in
  `i2c_hal_transport`, which is a firmware-only translation unit: what the host
  suite covers is the logic extracted out of it — the byte corrections in
  `SplitReadCompensator` (`split_read_test`, and a libFuzzer target over the
  same entry point) and the assertion accounting in `EdgeLatch`
  (`edge_latch_test`). That the transport calls each of them, at the one point
  in the sequence that earns it, is covered on the wire by `b-i2c100`.

  The split is not a choice. The length is knowable only from the header, and a
  BNO085 asked for more bytes than it holds stretches the clock until it has
  data (1000-3927 §1.2.2.2), so there is no safe over-read to predict with. SPI
  needs no split: one CSN assertion streams header and body as a single
  transfer.

  The third artifact is on the interrupt line rather than in the bytes. The hub
  asserts HINTN once per transfer and "deasserts and reasserts HINT for each
  transfer" of a cargo broken into several (1000-3535 §2.6), so a cargo
  retrieved in two transfers earns two assertions and the second announces the
  transfer just completed. The transport discards it. Left latched it outlives
  its assertion: the next cargo's read counts as edged on it, and its service
  latency is measured from it, reporting the transport's own read strategy
  rather than whether the loop keeps up (HR-5). Only the latch is cleared; a
  cargo the hub is genuinely holding still reaches `service()` through the level
  fallback (HR-1), so a discard can cost an anchor but never a cargo.

## 4. Performance requirements (PR)

Every rate below is gated against the hub's granted-rate band rather than
against the request: 1000-3927 §6.9 states
`0.9 * RequestedRate <= ConfiguredRate <= 2.1 * RequestedRate`, so a gate at
exactly the request would fail a hub behaving to specification, and a wider one
would pass a hub that is not. The band is stated once here and once in the
harness, and applies to `b-rate`, `b-rv400`, `b-rvphase`, `b-i2c100` and `b-girv`
alike.

- **PR-1 — 400 Hz rotation vector over SPI.** Sustained for 60 s with zero
  sequence gaps and zero drops, delivered inside the granted-rate band
  (`b-rv400`).
- **PR-2 — 100 Hz A+G+M over I2C.** Accelerometer, gyroscope, and magnetometer
  concurrently at 100 Hz over I2C at 400 kHz, 60 s, zero drops, each delivered
  inside the granted-rate band (`b-i2c100`).

- **PR-3 — Timestamp monotonicity.** Per-sensor timestamps are monotonic to the
  resolution of the fields they are reconstructed from, and jitter is p99 ≤ ±1
  report interval (`b-ts`). A time is the HINTN edge, captured in microseconds,
  less the base delta plus the report delay, both expressed in 100 µs ticks
  (1000-3625 §7.2.1 and 1000-3927 §1.3.5.3, Figure 1-36 worked example;
  the latter's prose disagrees with the example on the delay sign). Each field
  puts the time off by less than one
  tick, whether the hub truncates or rounds (UNCONFIRMED), so a time is off by
  less than two ticks and two times built separately differ by less than four.
  The reconstruction bound is therefore 400 µs: a backward step of 400 µs or
  more is a reconstruction fault and fails the gate, and anything smaller is
  quantisation the wire format cannot express away.

  A rotation vector's time is the hub's report of its sample: 1000-3927
  §1.3.5.3 times every report from its sensor interrupt, and §6.8 has a fused
  output follow a gyroscope sample. Neither document says which interrupt a
  fused report is timed from, nor that a rotation vector and the gyroscope
  delivered at one rate share a sample phase, and 1000-3625 §5.2 allows a
  report rate synchronous or asynchronous to the sensor's operating rate
  (UNCONFIRMED in the documents). PR-3 therefore gates no phase between the two
  streams. `b-rvphase` records the phase: it reports each rotation-vector
  time's offset from the nearest gyroscope time, which shows only modulo the
  interval, and passes when both are delivered at one rate over an interval of
  at least ten bounds.
  Rotation-vector times are reconstructed like any other sensor's, so `b-ts`
  and `b-rv400` gate their ordering at the reconstruction bound.
- **PR-4 — Gyro-integrated RV.** Requested at 500 Hz over SPI, delivered
  inside the granted-rate band stated above, with zero reassembly drops
  (`b-girv`).

## 5. Boundary requirements (BR)

- **BR-1 — Enabled-sensor cap.** Enabling a new sensor beyond the profile's
  `MAX_ENABLED_SENSORS` returns `UNSUPPORTED` and never evicts an active sensor;
  reconfiguring an already-enabled sensor is always accepted. Host-tested by
  `bno085_test`.
- **BR-2 — TX backpressure is surfaced, never silently absorbed.** A full
  session TX queue — pico-sh2 refuses a send it has no room to queue rather
  than dropping it — is reported to the caller rather than absorbed, and the desired table is left in the state that lets the request
  still be honoured. The two halves report it differently because they can
  offer the caller different guarantees:

  - An **enable** returns `EnableState::DEFERRED`, not an error. Its entry is
    recorded and a re-apply scheduled, so the Set Feature reaches the hub on a
    later `service()` once the queue drains, and the caller need do nothing —
    but a caller waiting to see events is told the hub has not been told yet.
  - A **disable** returns `NO_ROOM` and keeps its entry, so the sensor is never
    left streaming with no record from which to disable it. There is no
    deferred state to report, so the refusal is the caller's to retry.

  Host-tested by `bno085_test`.

## 6. Timing requirements (TR)

- **TR-1 — Deadline-bounded begin().** `begin()` is non-blocking; each startup
  phase (advertisement, reset-complete, initialize) is deadline-bounded, and
  on timeout faults with `TIMEOUT` and records the phase in `startup_phase()`.
  Host-tested by `bno085_test`.

## 7. Observability requirements (OR)

- **OR-1 — Stats counter set.** The driver exposes a normative `Stats` snapshot
  of the policy counters it alone owns: `hub_resets`, `recoveries`,
  `sensors_reenabled`, `config_rejections`, the current `enabled_sensors` count,
  and `diag_events` (OR-2). The invariant `recoveries ≤ hub_resets` holds, and
  the set is self-consistent under `FakeHub` reset injection (`bno085_test`).

  It is a separate accessor from the session's own counters rather than a
  merge of the two. `stats()` returns this set; `counters()` returns the
  pico-sh2 session's protocol and framing counters — cargoes in and out, events
  decoded, sequence gaps, reassembly drops, startup timeouts, resets — which
  pico-sh2's own observability requirement specifies and which this driver only
  passes through. Keeping them apart is what makes each counter answerable to
  the layer that defines it.

  The bench firmware surfaces both over USB CDC, alongside the transport's
  `level_fallback_reads` (HR-1), which the `b-ts` result reports so PR-3 jitter
  is attributable. Device-level metrics outside either set — the transport's
  absent reads and level fallbacks, the HINTN service gap, and post-init heap
  growth — are reported by the transports and the bench harness.

- **OR-2 — Diagnostic event count.** Every diagnostic event the session reports
  to the driver increments `Stats::diag_events`. Logging those events is rate
  limited, so a fault storm cannot put a log line in `service()` once per event;
  the count is not, because it is what a device run reads. A rate-limited
  counter would understate exactly the run that most needs measuring.

---

## 8. Traceability matrix

| Req | Test(s) | Pass criterion |
|-----|---------|----------------|
| AR-1 | `b-heap` (device); the footprint gate, in the `firmware` group | Heap growth after init is zero; separately, each example image's static RAM and flash are within budget |
| AR-2 | `edge_latch_test` (host API exercise, not tearing proof); review of the `pico_util:CR-2` seqlock choice and exchange-then-load order, `ConsumeAnchorsTheReadToTheEdgeLatchedBeforeIt`; review (the exchange-then-load order inside `consume()`, and every return of `i2c_hal_transport.cpp` and `spi_hal_transport.cpp` carrying `consume()`'s edge) | Review confirms a non-preemptible interrupt writer and an exchange before timestamp load; dispatch changes preserve the IRQ bank state, begin enables it, and end does not change it; a read is anchored to the edge `consume()` returns, latched before it clears the flag |
| AR-3 | `bno085_test` `ReEnablesDesiredSensorsAfterHubReset`, `ResetRestartsTheHandshakeAndReEnablesSensors`, `ReEnablesDesiredSensorsAfterAnUnacknowledgedRead` | Every desired sensor is re-applied after a hub reset, whether the re-handshake is drained over several calls or in one `service()`, and after a transport read error while READY |
| AR-4 | `bno085_test` — `MapsEachProtocolCodeToOneADriverCallerCanActOn`, `CollapsesTheFramingCodesADriverCallerCannotSeparate`; the compiler | Each `pico_sh2::Sh2Error` maps to exactly one `pico_bno085::Error`, with the four framing codes collapsed onto `PROTOCOL` and both capacity codes onto `NO_ROOM`; the switch names every enumerator and carries no `default:`, so an enumerator a later pico-sh2 appends is a `-Wswitch` error under `-Werror` |
| AR-5 | `consumer` gate case `pico-w-refused` in `house.toml` | A consumer adding the driver on `pico_w` fails at configure, saying `PICO_BNO085 needs PICO_PLATFORM rp2350` |
| HR-1 | `bno085_test` `ATransportReportingNoDataIsNeverRead`; `edge_latch_test` `ConsumeWithoutAnEdgeCountsALevelFallback`, `AnEdgeWhoseLineHasRisenIsNotLatched`, `AnEdgeWhoseLineIsStillAssertedIsLatched`, `ADroppedEdgeLeavesTheNextOneItsOwnSpan`; `b-boot` (device); review of `TransportCore::data_ready()` and the SPI write's discard | Through a transport counting its reads, a READY driver serviced 100 times with no data ready issues no read, and with a transfer waiting reads it. A read with no latched edge counts one level fallback, and an edged read counts none. An edge whose line has risen sets no flag, timestamp or span, and the next live edge opens its own. `data_ready()` reports a stashed duplex cargo first, otherwise the live level; a null-header write discards its answering assertion. `b-boot` shows only that the hub stays on the bus over a quiet interval: a blind poll of a present hub stretches the clock or reads a null header, which `absent_reads` does not count |
| HR-2 | `split_read_test` classification; `b-reset` (device) | `split_read_test` adjudicates it: `PICO_ERROR_GENERIC` is reported as hub absence and `PICO_ERROR_TIMEOUT` as a bus fault. `b-reset` exercises the absence path — the hub does leave the bus while resetting — but reads only that recovery completed, so it is a path exercise and not the criterion |
| HR-3 | `strap_plan_test` `I2cHoldsResetThenStrapsEveryLineSa0Last`, `Sa0TakesTheAddressLevelItIsGiven`, `SpiHoldsResetAndLeavesSa0ToTheTransport`; review of `control_pins.cpp`; `b-boot` (device) | NRST is asserted before any strap, PS1, PS0 and BOOTN take the selected bus's levels, and SA0 is driven last and only for I2C. That `ControlPins::init()` drives exactly the plan is review-only; `b-boot` shows the hub reaches READY, which a wrong strap state or a released NRST would deny |
| HR-4 | `b-parity` (device, pending final-root run); `DuplexWrite.KeepsCoincidentCargoAndPadsTheShorterDirection`, `BoundsLongCargoAndKeepsContinuation`, `NullAndReservedHeadersDoNotCreateCargo`; `b-rv400` mid-stream reconfiguration (device evidence pending); review of `spi_hal_transport.cpp` and `i2c_hal_transport.cpp` | `b-parity` shows the necessary condition: the SPI hub delivers a stream with no gaps or drops. Mode 3 (`SPI_CPOL_1, SPI_CPHA_1`), each `init()` refusing a requested or an actual clock above `SPI_MAX_BAUD_HZ` or `I2C_MAX_BAUD_HZ`, the WAKE → HINTN → CSN handshake and its bounded deadline, and PS0 taken as WAKE only after reset are review-only: the transports are firmware-only |
| HR-5 | `b-lat` (device) | Worst HINTN-to-read gap within budget, an order of magnitude under the hub's retry threshold |
| HR-6 | `split_read_test`; `split_read_fuzz`; `edge_latch_test`; `b-i2c100` (device) | The engine sees one sequence number per cargo and no continuation the host's own peek caused; a null header consumes neither; the assertion a split read's second transfer earns is dropped without consuming an edge or counting a level fallback |
| PR-1 | `b-rv400` (device) | Over SPI, which the case selects, the rotation vector requested at 400 Hz (1000-3927 Figure 6-16's maximum) is delivered inside the granted-rate band for 60 s with no gaps or drops |
| PR-2 | `b-i2c100` (device) | Accelerometer, gyroscope and magnetometer run concurrently over I2C for 60 s with no reassembly drops, no sequence gaps on any of the three, and each delivered rate inside the granted-rate band for a 100 Hz request |
| PR-3 | `b-ts` (device); `b-rvphase` (device) records the rotation-vector-to-gyroscope phase without gating it, and its optional `--wire` records independent timing reconstruction, feature grants and per-event driver agreement through full `trace 2` reads | No backward step of the four-tick reconstruction bound or more, and p99 jitter within one report interval |
| PR-4 | `b-girv` (device) | Over SPI, which the case selects, requested at 500 Hz, the delivered rate is inside the hub's granted-rate band and reassembly drops are zero |
| BR-1 | `bno085_test`: `EnableCapRejectsNewSensorsWithoutEviction`, `ReEnablingUpdatesInPlaceWithoutConsumingCap` | A sensor beyond the cap is refused and evicts nothing |
| BR-2 | `bno085_test` `EnableDefersWhenTheQueueIsFull`, `DisableKeepsDesiredEntryWhenQueueFull` | With the queue full, an enable returns `DEFERRED` and reaches the hub once it drains, and a disable returns `NO_ROOM` and keeps its entry |
| TR-1 | `bno085_test` `StartupDeadlineFaultsWithDistinctPhase`, `LaterStartupDeadlinesFaultWithTheirOwnPhase` | Each of the three startup deadlines faults with `TIMEOUT`, and `startup_phase()` names the phase |
| OR-1 | `bno085_test`: `StatsMirrorSessionAndTrackEnabled`, `SoakStatsRemainConsistentUnderResetInjection`; bench `stat` + `b-soak` (device) | Every counter moves only on the event its documentation names — `hub_resets` counts host-requested resets as well as spontaneous ones, because the re-advertisement that reports one carries no mark of its cause; a long run leaves the error counters at zero |
| OR-2 | `bno085_test` `EveryDiagnosticEventIsCounted` | Repeated events of one kind each increment the count, though only the first is logged |
