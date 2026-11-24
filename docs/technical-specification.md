# pico-bno085 Technical Specification

This specification is normative.

This specification defines the architecture, hardware, performance,
boundary, timing, observability and bench line protocol requirements for the
pico-bno085 BNO085 driver on the Raspberry Pi Pico 2 (RP2350). Every requirement has an ID
(`AR`/`HR`/`PR`/`BR`/`TR`/`OR`/`LP`) and is mapped to an acceptance test in the
traceability matrix. The platform-independent SH-2/SHTP protocol layer is
specified separately in [pico-sh2](https://github.com/jamesfxley/pico-sh2); this
document covers the device driver and its RP2350 transports. Measured device
results live in [examples/bench/baselines/](../examples/bench/baselines/).

A cargo is the payload carried by SHTP transfers (1000-3535 §2.0).

---

## 1. Scope and targets

| | |
|---|---|
| Hardware target | An RP2350 board. This repository builds for `pico2_w`, and the bench runs on a Pico 2 W; as a dependency the consumer's RP2350 board is taken as given, and running on a plain Pico 2 is UNCONFIRMED (FW-1). The BNO085 is wired per [usage-guide.md](usage-guide.md). The BNO086 is not validated: the code tolerates its zero-length-packet polling behaviour, but support for the part is UNCONFIRMED (FW-2). |
| Transports | I2C (≤400 kHz) and SPI (mode 3, ≤3 MHz), selected in firmware via the PS0/PS1 pins driven before reset. |
| Protocol | SH-2 over SHTP, via the `pico-sh2` dependency. |
| Out of scope | UART-RVC / UART-SHTP modes (FW-5); DFU / bootloader firmware update; the SH-2 protocol logic itself (specified in pico-sh2). |

Sizing follows the `PICO_BNO085_PROFILE` option (`default` / `large`), which
forwards to `PICO_SH2_PROFILE`.

The public API (`namespace pico_bno085`, headers under `pico_bno085/`) follows
[EN-06.1](../infra/house/standard/engineering/06-release-and-versioning.md).

---

## 2. Architecture requirements (AR)

- **AR-1 — No allocation after initialisation on device.** After
  `Bno085::begin()` completes, the driver performs no dynamic allocation.
  Verified on device by the bench suite's `HeapWatermark` growth counter reading
  zero (`b-heap`). Separately, both example images stay within <= 20 KiB of
  static RAM and <= 128 KiB of flash (all `LOAD` sections), which the footprint
  gate checks on every firmware build; it bounds the static footprint and cannot
  see an allocation. Both limits are checked because a fixed-capacity session
  buffer grows RAM and a report decoder grows flash.
  
- **AR-2 — The interrupt handler stores the edge and returns.** For each
  registered pin it identifies and acknowledges its edge, reads the timer and
  the live level, and, while the line is still asserted (HR-1), latches a flag
  and a microsecond timestamp through `EdgeLatch::latch`. It performs no bus
  I/O and allocates nothing. The work is bounded by construction rather than
  by a timing budget: a fixed loop over the dispatch table.

  The 64-bit timestamp is published under a seqlock, so a `service()`-context
  reader never observes a value torn across the two 32-bit stores the M33
  emits. The fences are full thread fences, not signal fences; no cross-core
  guarantee is stated for the rest (FW-3).

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
- **AR-4 — Driver-owned error taxonomy.** Every fallible
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
- **AR-5 — A board without an RP2350 fails configuration.** pico-logging's
  sink and level atomics need the RP2350's exclusive access, so the driver
  declares `PLATFORM rp2350` to pico-tooling's bootstrap, which fails
  configuration for a board without the declared platform. A consumer adding it
  on an RP2040 board fails at configure, naming the platform (FW-1).

## 3. Hardware requirements (HR)

- **HR-1 — HINTN-gated reads.** A bus `read_transfer` begins only while HINTN is
  asserted (a stashed duplex cargo needs no bus activity): never on a blind
  schedule, and never on a latched edge alone. The
  hub asserts HINT when it has a cargo to be read and deasserts it after the
  read begins (1000-3535 §2.6), so the live level indicates a waiting cargo and
  the latched edge records its assertion time (AR-2); a missed or coalesced edge
  cannot wedge `service()`, since the read does not wait on one. The level is
  only as current as the hub's deassert: §2.6 lets it lag "including after the
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
  reasserts complies is UNCONFIRMED (FW-2); a BNO085 not ready for it
  stretches the clock, which HR-2's timeout bounds. An edge can outlive
  its assertion: a handler held off past the read that answered it runs after
  it. The handler therefore drops an edge whose line has already risen, and a
  transport clears the latch for an assertion answered by its completed
  transfer: a split read's continuation (HR-6) and a null-header SPI wake reply.
  A duplex cargo instead retains the answering edge for the stashed read. An
  edge whose handler runs after the read begins but before the hub deasserts,
  typically 10 µs on I2C and 800 ns on SPI (1000-3927 Figure 6-12, tclid and
  tcsid), is still latched; it causes no read, but the next read counts as edged
  on it, and, since a later edge does not move a span already open, that read's
  service latency is measured from it (HR-5), early by up to one report
  interval. A BNO085
  polled with no data available stretches the I2C clock indefinitely and a
  BNO086 returns zero-length packets (1000-3927 §1.2.2.2: a polled BNO085 with
  nothing to send "will stretch the clock until it has data", while a BNO086
  "will send zero data packets" the host is told to read the length of and
  ignore); the driver relies on neither and tolerates both by passing empty and
  dead-bus reads to the engine without a transport fault, and the session bounds
  the transfers drained per `service()` so a stuck-low line cannot spin it. A
  read with no latched edge behind it, a level fallback, is anchored (AR-2) to
  the edge latched when it clears the flag: an earlier edge, unless the handler
  latched the fresh one in between, and the events it carries are timed from
  that. The HAL counts these reads (`level_fallback_reads`), an upper bound on
  the fallback reads anchored early, so a device run can bound how often it
  occurs and attribute any PR-3 jitter to it. Implemented in the HAL transports.
- **HR-2 — Timeout-bounded I2C, and an unacknowledged read is absence, not a
  fault.** Every I2C transaction uses the SDK's `*_timeout_us` variants; a
  stretch or bus fault surfaces `TIMEOUT`/`BUS_ERROR` and never hangs
  `service()`. The two SDK failure returns carry different meanings and are not
  conflated: `PICO_ERROR_TIMEOUT` is a stretched or wedged bus and is a fault,
  while `PICO_ERROR_GENERIC` is "address not acknowledged, no device present"
  and means the hub has left the bus. Because a read is only issued behind a
  HINTN assertion (HR-1), a hub that does not acknowledge one has left since
  asserting, which the transport treats as a reset in progress. The transport
  reports it as the zero-length transfer a hub that cannot answer produces — the
  only thing a transport with no acknowledge bit can report — and counts it in
  `absent_reads`; recovery is the session's (pico-sh2 SR-5). The split-read
  state the transport carries across transfers (continuation and per-channel
  peek-sequence compensation, HR-6) is cleared with it, since the hub abandons
  any part-sent cargo and restarts its channel sequence numbers while away.
  Implemented in `i2c_hal_transport`.
- **HR-3 — Reset sequencing.** The straps the selected bus requires are driven
  before NRST is released; the session holds NRST low for its `reset_hold_us`
  (2 ms, against a `tnrst` minimum of 10 ns in 1000-3927 §6.5.3 —
  a factor of 200000 in margin, chosen for the hub's boot rather than the
  pin) and bounds the boot and advertisement
  phases by its own deadlines. PS0/PS1/BOOTN are driven in both modes; SA0 is an
  I2C-only strap because the hub multiplexes it with SPI data-in (SA0/H_MOSI),
  so in SPI mode the pin is left to the SPI transport and no strap driver can
  contend with it on a shared net. Implemented in `control_pins`, which drives
  the steps `detail::strap_plan` orders.
- **HR-4 — SPI handshake.** SPI runs mode 3 at ≤ 3 MHz; a host-initiated write
  performs the WAKE → HINTN → CSN handshake with a bounded deadline; PS0 is
  repurposed as WAKE only after reset. Neither transport runs its bus above
  the hub's limit: `init()` rejects a requested clock above 3 MHz for SPI
  (1000-3927 §6.5.2) or 400 kHz for I2C (§6.5.1) with `INVALID_ARGUMENT`
  before any pin is touched, and returns the same error after releasing
  the peripheral if the actual SDK rate exceeds the limit. `init()` also
  returns `INVALID_ARGUMENT`, before touching pins, for a GPIO outside
  bank 0 and an SDA, SCL, MISO, SCK or MOSI pin the selected peripheral cannot
  reach under the RP2350 pin mux (datasheet Table 677); CSN and HINTN are
  plain GPIOs and only the bank bound applies. `HintnLatch::begin()` refuses
  a HINTN pin outside bank 0 the same way. A HINTN pin from 32 to 47 on a
  48-GPIO RP2350B is UNCONFIRMED: this repository builds only for `pico2_w`. `detail::pin_mux` holds those rules. Implemented
  in `spi_hal_transport` and `i2c_hal_transport`. A write receives the
  coincident hub cargo or null header in the same duplex transfer, padding the
  shorter direction (1000-3535 §3.3, §2.3.1). The next read returns that stashed
  cargo with the answering edge.

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
  would under-report the latency of a delayed service pass.
  `EdgeLatch` holds the span start alongside the newest edge for this reason.

  The window matters too: startup spends the reset hold and boot delay in timed
  waits by design, so an edge arriving mid-wait is serviced late for a reason
  that says nothing about the steady-state loop. A cumulative mark would report
  that startup figure forever.

  The anchor is a separate question from the measurement. A read served by the
  level fallback may anchor its events to an earlier edge (HR-1). A read that
  answers a retried assertion anchors to the retry's edge; whether the hub
  re-takes the cargo's base delta against the retry is UNCONFIRMED (FW-2;
  1000-3927 §1.2.4.1 says only that it deasserts and retries).
- **HR-6 — I2C split reads are invisible above the HAL.** The I2C transport
  retrieves a cargo by reading the 4-byte header to learn its length and then
  reading the cargo itself, the split read SHTP defines for a host that does not
  know the length in advance (1000-3535 §2.3.1). The hub answers such a read by
  re-offering the cargo with the length field's continuation bit set, and it
  increments the channel's sequence number once per read, so a cargo retrieved
  this way consumes two. Both result from the transport's split read, not
  from the cargo: the transport clears the continuation
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

  The length is available only in the header. A
  BNO085 asked for more bytes than it holds stretches the clock until it has
  data (1000-3927 §1.2.2.2), so there is no safe over-read to predict with. SPI
  needs no split: one CSN assertion streams header and body as a single
  transfer.

  The split read also produces a second HINTN assertion. The hub
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
  (1000-3625 §7.2.1 and 1000-3927 §1.3.5.3, Figure 1-36 worked example; the
  latter's prose disagrees with the example on the delay sign). Each field puts
  the time off by less than one tick, whether the hub truncates or rounds
  (UNCONFIRMED, FW-2), so a time is off by less than two ticks and two times
  built separately differ by less than four. The reconstruction bound is
  therefore 400 µs: a backward step of 400 µs or more is a reconstruction fault
  and fails the gate, and smaller steps fall within the wire format's
  quantisation bound.

  A rotation vector's time is the hub's report of its sample: 1000-3927
  §1.3.5.3 times every report from its sensor interrupt, and §6.8 has a fused
  output follow a gyroscope sample. Neither document says which interrupt a
  fused report is timed from, nor that a rotation vector and the gyroscope
  delivered at one rate share a sample phase, and 1000-3625 §5.2 allows a
  report rate synchronous or asynchronous to the sensor's operating rate
  (UNCONFIRMED in the documents, FW-2). The phase between the two streams is
  therefore not gated (PR-3). `b-rvphase` records the phase: it reports each
  rotation-vector time's offset from the nearest gyroscope time, which shows
  only modulo the interval, and passes when both are delivered at one rate over
  an interval of at least ten bounds.
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
  session TX queue — pico-sh2 returns QUEUE_FULL rather than dropping a send it
  cannot enqueue
   — is reported to the caller rather than absorbed, and the desired table is
   left in the state that lets the request
  still be honoured. The two halves report it differently because they can
  offer the caller different guarantees:

  - An **enable** returns `EnableState::DEFERRED`, not an error. Its entry is
    recorded and a re-apply scheduled, so the Set Feature reaches the hub on a
    later `service()` once the queue drains, and the caller need do nothing —
    but a caller waiting to see events is told the hub has not been told yet.
  - A **disable** returns `NO_ROOM` and keeps its entry, so the sensor is never
    left streaming with no record from which to disable it. There is no
    deferred state to report, so retrying a failed disable is left to the
    caller.

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
  the count includes every event so device measurements do not under-report a
  fault storm.

## 8. Bench line protocol requirements (LP)

The bench firmware (`examples/bench/main.cpp`) and its harnesses speak a line
protocol over USB CDC. The firmware's formatters and argument parsers are in
`examples/bench/bench_protocol.hpp`; the firmware and the host test
`pico_bno085_bench_tests` include that one header, so the test exercises the
bytes the firmware prints. The SH-2 manual is 1000-3625 and the datasheet
1000-3927.

- **LP-1 — Framing.** Every line is ASCII and ends in a line feed. A command
  ends at a line feed or a carriage return; an empty line is ignored. The
  firmware keeps the first 64 characters of a line, runs them as the command
  and drops the rest of the line.
  A reply begins `# ` and carries space-separated words or `key=value` pairs.
  A telemetry line begins with one tag letter and a comma, and its fields are
  comma-separated with no spaces. A telemetry line or command response is at
  most 127 characters.
- **LP-2 — Commands.** A command is a word and its arguments, separated by
  spaces. Every command except `bus`, `info` and `payload` needs a bus
  selected, and is otherwise answered `# err no bus selected`. Whether a line is a command is decided by its first word, which ends at the
  first space. A line whose first word is none of those below, or is one that
  takes no arguments and is followed by any text, even a space, is answered
  `# err unknown command`. A word that takes arguments, given none or arguments
  that do not parse, is answered `# err parse`. The bus check comes before both.

  | Command | Arguments | Does |
  |---|---|---|
  | `bus` | `i2c` or `spi` | Brings up the bus with the hub held in reset |
  | `info` | | Prints the constants the harness needs |
  | `payload` | `0` or `1` | Sets payload mode (LP-5) |
  | `cfg` | `<id_hex> <us>` | Enables or reconfigures a sensor at a report interval |
  | `off` | `<id_hex>` | Disables a sensor and removes its desired entry |
  | `flush` | `<id_hex>` | Asks a batching sensor to deliver now |
  | `run` | `<ms>` | Prints telemetry for that long |
  | `stat` | | Prints the counters |
  | `heap` | | Prints post-initialisation heap growth |
  | `reset` | | Restarts the hub through the executable channel |
  | `cleardcd` | | Clears dynamic calibration data and resets the hub |
  | `trace` | `0`, `1` or `2` | Sets the wire trace mode |
  | `boot` | | Runs the handshake again |
  | `tare` | `<axes_hex> <basis>` | Tares now. Axes are `7` (X, Y and Z) or `4` (Z alone); basis is `0` to `5`, the rotation vector the tare is taken against (1000-3625 §6.4.3.1) |
  | `tarepersist` | | Saves the last tare to flash (1000-3625 §6.4.3.2) |
  | `cal` | `<accel> <gyro> <mag>` | Sets each calibration routine to `0` (off) or `1` (on) (1000-3625 §6.4.6.1) |
  | `calget` | | Reads the state of the calibration routines (1000-3625 §6.4.6.2) |
  | `savedcd` | | Saves dynamic calibration data to flash (1000-3625 §6.4.5) |
  | `pdcd` | `0` or `1` | Turns periodic saving of the data off or on (1000-3625 §6.4.7.1) |

  `cal` also sets the planar-accelerometer and on-table routines off, because
  the driver writes zero to both parameters (1000-3625 §6.4.6.1). `cfg` and
  `off` take the sensor as the SH-2 report ID in hexadecimal. `tare`,
  `tarepersist`, `cal`, `calget`, `savedcd` and `pdcd` need the session READY,
  and the driver refuses them `not_ready` otherwise. `bus`, `payload`, `cfg`,
  `off`, `flush`, `run`, `trace`, `tare`, `cal` and `pdcd` take arguments; the
  other words take none.
- **LP-3 — Replies.** Each command is answered by the reply below. A command
  the driver refuses (`tare`, `tarepersist`, `cal`, `calget`, `savedcd`,
  `pdcd`) is answered `# err <name>`, the name being the driver error in lower
  case with underscores: `bus_error`, `timeout`, `not_ready`, `protocol`,
  `busy`, `no_room`, `unsupported` or `invalid_argument`. The older commands
  answer a refusal with a bare `# err`.

  | Command | Reply |
  |---|---|
  | `bus` | `# bus <i2c\|spi>`, or `# err bus must be i2c or spi`, or `# err transport init` |
  | `info` | `# info tick_us=<n> max_enabled=<n> bus=<i2c\|spi\|none>` |
  | `payload` | `# payload <0\|1>` |
  | `cfg`, `off`, `flush` | `# ok` or `# err` |
  | `run` | `# run <ms>`, then telemetry for the time, then `# stats …` and `# done events=<n>` |
  | `stat` | `# stats bus=… ready=… state=… fault=… fault_phase=…` and the session's counters as `key=value` pairs |
  | `heap` | `# heap armed=<0\|1> growth=<bytes>` |
  | `reset`, `cleardcd` | `# reset` or `# cleardcd`, or `# err` |
  | `trace` | `# trace <n>` |
  | `boot` | `# boot` |
  | `tare`, `tarepersist`, `cal`, `calget`, `savedcd`, `pdcd` | `# ok` or `# err <name>` |

  At power-up the firmware prints `# ready-for-bus` and an `# info` line, and
  it prints `# ready` each time the session reaches READY. With trace on it
  prints `# tx`, `# rx` and `# wire` lines of raw transfers.
- **LP-4 — The E line.** An event prints
  `E,<sensor>,<seq>,<event_us>,<accuracy>`: the SH-2 report ID in decimal; the
  sensor's own report sequence number, 0 to 255; the event time in decimal
  microseconds on the board's clock, reconstructed as PR-3 states; and the
  accuracy, 0 (unreliable), 1 (low), 2 (medium) or 3 (high), the status bits of
  1000-3927 §1.3.5.2. Events print only between `run` and its `# done`.
- **LP-5 — Payload mode.** `payload 1` makes each event whose sample has a
  payload line (LP-6) print that line in place of its E line. Every other event
  keeps its E line. `payload 0`, the state at power-up, makes every event print
  its E line, and the bytes are those the bench printed before payload mode
  existed. The setting persists until the next `payload` command or power
  cycle, so a harness that counts E lines sends `payload 0` first.

  A payload line replaces the E line, not follows it, because it begins with
  the E line's four fields and so carries all of them: one line per event keeps
  the byte rate down by the length of an E line (about a quarter), needs no
  pairing of two lines, and leaves an event atomic on the wire. A reader takes
  the four common fields from any telemetry line and the tag says what follows.
- **LP-6 — Payload lines.** A payload line has the four common fields of
  LP-4 after its tag, then the fields below, whose tag the sample's kind
  selects (the sensor ID does not).

  | Tag | Sample | Sensors | Fields after the common four |
  |---|---|---|---|
  | `Q` | Rotation with accuracy | `05`, `09`, `28` | `w,i,j,k,acc_rad` |
  | `R` | Rotation without accuracy | `08`, `29` | `w,i,j,k` |
  | `V` | Three-axis vector | `01`, `02`, `03`, `04`, `06` | `x,y,z` |
  | `U` | Uncalibrated vector and bias | `07`, `0F` | `x,y,z,bx,by,bz` |
  | `G` | Gyro-integrated rotation | `2A` | `w,i,j,k,wx,wy,wz` |

  Units. `w,i,j,k` is a unit quaternion, dimensionless, with the scalar part
  first (the SH-2 reports send it last; the line does not). `acc_rad` is the heading
  accuracy estimate in radians (§6.5.18). `V` carries acceleration including
  gravity (`01`), linear acceleration (`04`) and gravity (`06`) in m/s²,
  calibrated angular velocity (`02`) in rad/s, and calibrated magnetic field
  (`03`) in µT (§§6.5.9 to 6.5.16). `U` carries the uncalibrated value and the
  bias the hub removed to obtain the calibrated one, in rad/s for `07` and µT
  for `0F` (§§6.5.14, 6.5.17). `wx,wy,wz` is the calibrated angular velocity in
  rad/s that `G`'s orientation was integrated from (§6.5.44).

  Frames. Vector axes are the device frame, which the hub aligns to the
  configured sensor orientation; its default is X to the right, Y along the
  face and Z out of the front face (1000-3927 §2.1, Figure 2-1). A tare
  reorients every motion output (1000-3625 §6.4.3.1). `Q` (`05`) is referenced
  to magnetic north and gravity (1000-3927 §2.2.4). `R` is referenced to
  gravity alone and has no heading reference (§2.2.2). The direction of the
  rotation a quaternion expresses, the axes of the reference frame, and which
  reference `G` takes by default, are UNCONFIRMED in 1000-3625 §§6.5.18 and
  6.5.44 and 1000-3927 §§2.2.4 and 2.2.6 (FW-2).

  Number format. Every payload number is a decimal with exactly five digits
  after the point, rounded to nearest with ties away from zero, with no
  exponent and no `+`. A negative value that rounds to zero prints without its
  sign. A value that is not finite, or whose magnitude is 1e9 or more, prints
  `nan`; no decoded field can reach either. The hub's finest step
  is one part in 2^14 for a quaternion (1000-3625 §6.5.18), 6.1e-5, so five
  digits lose nothing. pico-sh2 delivers each field already converted to a
  float in these units, and the bench does not read the report bytes again, so
  a line of raw integers would need each field's Q point beside it, and the
  reader would convert. LP-8 shows the decimals fit the link. The formatter
  writes the digits with integer arithmetic, so the result does
  not depend on a `printf` float implementation, and the host test and the
  firmware print the same bytes.
- **LP-7 — Command responses.** A command accepted by `tare`, `tarepersist`,
  `cal`, `calget` or `savedcd` starts a wait for the hub's response to its SH-2
  command: Tare for `tare` and `tarepersist`, ME calibration for `cal` and
  `calget`, Save DCD for `savedcd`. The first response to that command ends the
  wait and prints as one line. A response to any other command, including the
  Initialize response after each reset, prints nothing, so output with no such
  command in flight is unchanged. `pdcd` has no response (1000-3625 §6.4.7)
  and starts no wait.

  At most one wait exists, and a newly accepted command replaces it. A wait
  ends with nothing printed when the session's command timeout expires (the
  `command_timeouts` count in `# stats` rises), when `reset`, `cleardcd` or
  `boot` runs, and when a command is refused with any error but `busy`. A
  refusal with `busy` leaves the wait of the command still in flight. The bench
  cannot tell an unsolicited response from a solicited one, because pico-sh2
  clears the unsolicited flag before it delivers the response; the only
  unsolicited response known is Initialize, which no wait is for. The lines
  are:

  | Response | Line |
  |---|---|
  | Save DCD | `# dcd status=<r0>`, 0 for success (§6.4.5.2) |
  | ME calibration | `# cal status=<r0> accel=<r1> gyro=<r2> mag=<r3> planar=<r4> table=<r5>`, status 0 for success and each routine 1 for on (§6.4.6.3) |
  | Any other | `# resp cmd=<hex> seq=<n> rseq=<n> r=<r0..r10>`: the command in two hex digits, the request's sequence number, the response's sequence number within its group, and the eleven result bytes as twenty-two hex digits, first byte first |

  A tare response is printed in the last form. The manual defines the tare
  command and names a response, but gives no layout for it (§6.4.3), so the
  meaning of its bytes is UNCONFIRMED (FW-2). The get-calibration reply is
  decoded with the layout of §6.4.6.3, which the manual gives for the configure
  command's response; that the get command answers with the same layout is
  UNCONFIRMED (FW-2). A command the hub never answers prints no line; the
  session's command timeout then frees the command slot and adds to
  `command_timeouts` in `# stats`.
- **LP-8 — Bandwidth.** With payload on, every sensor the bench can enable
  that prints a payload line, enabled at its maximum rate, prints at most
  381140 bytes a second, under half of the full-speed bulk ceiling of 1216000
  bytes a second (nineteen 64-byte packets in each 1 ms frame, USB 2.0 bulk
  transfers). The table is the arithmetic: each line is the longest its form prints, with a
  20-digit time, sequence 255, the sensor ID in decimal and each field at the
  widest value its Q point allows, newline included.

  | Sensor | Line | Bytes | Maximum rate (Hz) | Bytes a second |
  |---|---|---|---|---|
  | Gyro-integrated rotation (`2A`) | `G` | 98 | 1000 | 98000 |
  | Rotation vector (`05`) | `Q` | 76 | 400 | 30400 |
  | AR/VR stabilised rotation vector (`28`) | `Q` | 77 | 400 | 30800 |
  | Game rotation vector (`08`) | `R` | 67 | 400 | 26800 |
  | AR/VR stabilised game rotation vector (`29`) | `R` | 68 | 400 | 27200 |
  | Geomagnetic rotation vector (`09`) | `Q` | 76 | 90 | 6840 |
  | Gravity (`06`) | `V` | 64 | 400 | 25600 |
  | Linear acceleration (`04`) | `V` | 64 | 400 | 25600 |
  | Accelerometer (`01`) | `V` | 64 | 500 | 32000 |
  | Gyroscope (`02`) | `V` | 61 | 400 | 24400 |
  | Uncalibrated gyroscope (`07`) | `U` | 91 | 400 | 36400 |
  | Magnetometer (`03`) | `V` | 67 | 100 | 6700 |
  | Uncalibrated magnetometer (`0F`) | `U` | 104 | 100 | 10400 |
  | Total | | | | 381140 |

  The rates are the maxima of 1000-3927 Figure 6-16 for the nine sensors it
  lists. The stabilised rotation vectors take the rates of their
  non-stabilised versions (§6.9). Figure 6-16 does not list the uncalibrated
  gyroscope and magnetometer; the table gives them the rates of the calibrated
  sensors, which is UNCONFIRMED.

  The bound exceeds what can run. The datasheet says the sensors cannot all run
  at their maxima together (§6.9), and the default profile enables at most
  `MAX_ENABLED_SENSORS` of them (BR-1). Rotation vector alone at 400 Hz is
  30400 bytes a second. The bound is against the bus's ceiling, not against the
  rate the board's USB stack sustains, which is UNCONFIRMED until a device run
  measures it (FW-7); USB 2.0 is not among the local references, so the
  ceiling's source is UNCONFIRMED against them.

---

## 9. Traceability matrix

| Req | Test(s) | Pass criterion |
|-----|---------|----------------|
| AR-1 | `b-heap` (device); the footprint gate, in the `firmware` group | Heap growth after init is zero; separately, each example image's static RAM and flash are within budget |
| AR-2 | `edge_latch_test` (host API exercise, not tearing proof); review of the `pico_util:CR-2` seqlock choice and exchange-then-load order, `ConsumeAnchorsTheReadToTheEdgeLatchedBeforeIt`; review (the exchange-then-load order inside `consume()`, and every return of `i2c_hal_transport.cpp` and `spi_hal_transport.cpp` carrying `consume()`'s edge) | Review confirms a non-preemptible interrupt writer and an exchange before timestamp load; dispatch changes preserve the IRQ bank state, begin enables it, and end does not change it; a read is anchored to the edge `consume()` returns, latched before it clears the flag |
| AR-3 | `bno085_test` `ReEnablesDesiredSensorsAfterHubReset`, `ResetRestartsTheHandshakeAndReEnablesSensors`, `ReEnablesDesiredSensorsAfterAnUnacknowledgedRead`; `tests/device/main.cpp` — `hub-reset-restores-desired-sensor` | Every desired sensor is re-applied after a hub reset, whether the re-handshake is drained over several calls or in one `service()`, and after a transport read error while READY. Device case: on default I2C, READY and a rotation-vector event arrive under separate 3 s guards; after one non-destructive `reset()` request, the reset, recovery and sensor-reapply counters advance, READY returns and a fresh rotation-vector event arrives within a shared 3 s guard, without another enable request. Guards detect a stalled case, not recovery latency. All device cases share a 14 s service-loop guard, leaving room for a bounded service call and console output within 20 s of console attachment |
| AR-4 | `bno085_test` — `MapsEachProtocolCodeToOneADriverCallerCanActOn`, `CollapsesTheFramingCodesADriverCallerCannotSeparate`; the compiler | Each `pico_sh2::Sh2Error` maps to exactly one `pico_bno085::Error`, with the four framing codes collapsed onto `PROTOCOL` and both capacity codes onto `NO_ROOM`; the switch names every enumerator and carries no `default:`, so an enumerator a later pico-sh2 appends is a `-Wswitch` error under `-Werror` |
| AR-5 | `consumer` gate case `pico-w-refused` in `house.toml` | A consumer adding the driver on `pico_w` fails at configure, saying `PICO_BNO085 needs PICO_PLATFORM rp2350` |
| HR-1 | `bno085_test` `ATransportReportingNoDataIsNeverRead`; `edge_latch_test` `ConsumeWithoutAnEdgeCountsALevelFallback`, `AnEdgeWhoseLineHasRisenIsNotLatched`, `AnEdgeWhoseLineIsStillAssertedIsLatched`, `ADroppedEdgeLeavesTheNextOneItsOwnSpan`; `b-boot` (device); review of `TransportCore::data_ready()` and the SPI write's discard | Through a transport counting its reads, a READY driver serviced 100 times with no data ready issues no read, and with a transfer waiting reads it. A read with no latched edge counts one level fallback, and an edged read counts none. An edge whose line has risen sets no flag, timestamp or span, and the next live edge opens its own. `data_ready()` reports a stashed duplex cargo first, otherwise the live level; a null-header write discards its answering assertion. `b-boot` shows only that the hub stays on the bus over a quiet interval: a blind poll of a present hub stretches the clock or reads a null header, which `absent_reads` does not count |
| HR-2 | `split_read_test` classification; `b-reset` (device) | `split_read_test` adjudicates it: `PICO_ERROR_GENERIC` is reported as hub absence and `PICO_ERROR_TIMEOUT` as a bus fault. `b-reset` exercises the absence path — the hub does leave the bus while resetting — but reads only that recovery completed, so it is a path exercise and not the criterion |
| HR-3 | `strap_plan_test` `I2cHoldsResetThenStrapsEveryLineSa0Last`, `Sa0TakesTheAddressLevelItIsGiven`, `SpiHoldsResetAndLeavesSa0ToTheTransport`; review of `control_pins.cpp`; `b-boot` (device); `tests/device/main.cpp` — `default-straps-boot-each-bus` | NRST is asserted before any strap, PS1, PS0 and BOOTN take the selected bus's levels, and SA0 is driven last and only for I2C. That `ControlPins::init()` drives exactly the plan is review-only; the device case initialises the documented default I2C configuration, services until READY and ends it, then does the same for SPI, each under a 3 s guard. READY exercises the reset/strap path; it does not establish strap levels or reset pulse width. The guard detects a stalled case, not boot latency |
| HR-4 | `b-parity` (device, pending final-root run); `pin_mux_test` `SpiRoleAndInstanceMatchTable677ForEveryGpio`, `I2cRoleAndInstanceMatchTable677ForEveryGpio`, `ASmallBankRefusesEveryGpioFromThirty`, `EveryGpioBeyondTheBankIsRefusedWhateverTheRole`, `AnInstanceTheChipLacksIsRefused`, `TheDefaultPinsAreAcceptedForTheirInstances`; `DuplexWrite.KeepsCoincidentCargoAndPadsTheShorterDirection`, `BoundsLongCargoAndKeepsContinuation`, `NullAndReservedHeadersDoNotCreateCargo`; `b-rv400` mid-stream reconfiguration (device evidence pending); `tests/device/main.cpp` — `i2c-refuses-swapped-sda-scl`, `i2c-refuses-the-other-instance`, `i2c-refuses-a-pin-outside-bank-0`, `i2c-refuses-a-hintn-pin-outside-bank-0`, `spi-refuses-swapped-miso-mosi`, `spi-refuses-the-other-instance`, `spi-refuses-a-pin-outside-bank-0`, `spi-refuses-a-hintn-pin-outside-bank-0`, `i2c-accepts-the-default-pins`, `spi-accepts-the-default-pins`; review of `spi_hal_transport.cpp`, `i2c_hal_transport.cpp` and `hintn_irq.cpp` | `b-parity` shows the necessary condition: the SPI hub delivers a stream with no gaps or drops. Mode 3 (`SPI_CPOL_1, SPI_CPHA_1`), each `init()` rejecting a requested or actual clock above `SPI_MAX_BAUD_HZ` or `I2C_MAX_BAUD_HZ`, the WAKE → HINTN → CSN handshake and its bounded deadline, and PS0 taken as WAKE only after reset are review-only: the transports are firmware-only. `pin_mux_test` checks the pin-mux predicates against a table transcribed from the datasheet; the eight refusal cases check `INVALID_ARGUMENT` and unchanged function select and pad isolation for every supplied in-bank pin, without reading out-of-bank GPIO. They observe this subset of untouched pin state, not every register or pad field. The two default-pin cases each initialise, reach READY under a 3 s guard and end; the guard detects a stall, not boot latency. `pins.hpp` binds its default pins to the predicates at compile time |
| HR-5 | `b-lat` (device) | Worst HINTN-to-read gap within budget, an order of magnitude under the hub's retry threshold |
| HR-6 | `split_read_test`; `split_read_fuzz`; `edge_latch_test`; `b-i2c100` (device) | The engine sees one sequence number per cargo and no continuation the host's own peek caused; a null header consumes neither; the assertion a split read's second transfer earns is dropped without consuming an edge or counting a level fallback |
| PR-1 | `b-rv400` (device) | Over SPI, which the case selects, the rotation vector requested at 400 Hz (1000-3927 Figure 6-16's maximum) is delivered inside the granted-rate band for 60 s with no gaps or drops |
| PR-2 | `b-i2c100` (device) | Accelerometer, gyroscope and magnetometer run concurrently over I2C for 60 s with no reassembly drops, no sequence gaps on any of the three, and each delivered rate inside the granted-rate band for a 100 Hz request |
| PR-3 | `b-ts` (device); `b-rvphase` (device) records the rotation-vector-to-gyroscope phase without gating it, and its optional `--wire` records independent timing reconstruction, feature grants and per-event driver agreement through full `trace 2` reads | No backward step of the four-tick reconstruction bound or more, and p99 jitter within one report interval |
| PR-4 | `b-girv` (device) | Over SPI, which the case selects, requested at 500 Hz, the delivered rate is inside the hub's granted-rate band and reassembly drops are zero |
| BR-1 | `bno085_test`: `EnableCapRejectsNewSensorsWithoutEviction`, `ReEnablingUpdatesInPlaceWithoutConsumingCap` | A new sensor beyond the cap returns UNSUPPORTED and evicts nothing |
| BR-2 | `bno085_test` `EnableDefersWhenTheQueueIsFull`, `DisableKeepsDesiredEntryWhenQueueFull` | With the queue full, an enable returns `DEFERRED` and reaches the hub once it drains, and a disable returns `NO_ROOM` and keeps its entry |
| TR-1 | `bno085_test` `StartupDeadlineFaultsWithDistinctPhase`, `LaterStartupDeadlinesFaultWithTheirOwnPhase` | Each of the three startup deadlines faults with `TIMEOUT`, and `startup_phase()` names the phase |
| OR-1 | `bno085_test`: `StatsMirrorSessionAndTrackEnabled`, `SoakStatsRemainConsistentUnderResetInjection`; bench `stat` + `b-soak` (device) | Every counter changes only for its documented event. `hub_resets` counts host-requested resets as well as spontaneous ones, because the re-advertisement that reports one carries no mark of its cause; a long run leaves the error counters at zero |
| OR-2 | `bno085_test` `EveryDiagnosticEventIsCounted` | Repeated events of one kind each increment the count, though only the first is logged |
| LP-1 | `pico_bno085_bench_tests`: `BenchProtocol.ABufferThatIsTooSmallYieldsNoLine`, `WorstEnabledSetFitsHalfTheFullSpeedBulkCeiling`, `SequenceNumbersAboveOneHundredTwentySevenPrintInFull`; review of `poll_line` in `examples/bench/main.cpp` | A formatter writes no more than its buffer and returns no line if it does not fit; the widest line is 104 bytes against a 127-character limit; the command-length and terminator rules are review |
| LP-2 | `BenchProtocol.FlagArgumentsAreZeroOrOneAlone`, `TareTakesSupportedAxesAndABasis`, `CalibrationTakesThreeFlags`, `EveryWordSelectsItsCommand`, `AWordWithoutItsArgumentsIsRecognisedForParsing`, `AMalformedRequestIsMalformedAndAnotherCommandIsNot`, `EachCommandMakesItsDriverCall`, `PeriodicDcdPassesItsFlagThrough`, `CalibrationGetSendsSubcommandOneOfMeCalibration`, `PayloadIsOffAtPowerUp`; `tests/bench/test_payload.py` `test_a_session_disables_payload_before_it_selects_the_bus`; `b-boot`, `b-rate` and the other cases (device) for the older commands | Each word selects its command, a word that takes arguments is recognised without them and a word that takes none is not, each argument parser accepts exactly the forms of LP-2 and refuses the rest, each request makes its driver call with the parameters stated, the power-up payload mode is off, and the harness sends `payload 0` before `bus`; the bus check that precedes the parse and the print of each reply are review of `handle_command`, and a device run drives the older commands |
| LP-3 | `BenchProtocol.EveryDriverErrorHasItsLp3Name`; `tests/bench/test_payload.py` `test_payload_is_switched_off_and_acknowledged`, `test_a_reply_that_is_not_the_acknowledgement_is_reported`; `b-boot`, `b-rate` (device) | Each driver error has exactly the name LP-3 lists; the harness accepts only the `# payload 0` reply; a device run reads the other replies |
| LP-4 | `BenchProtocol.PayloadOffPrintsTheLegacyLineForEveryKindAndHeader`; `b-rate`, `b-ts` (device) | For every sample kind, sensor, time and accuracy, `format_event` with payload off equals a reconstruction of the original `printf("E,%u,%u,%llu,%u\n", ...)`, written in the test with `std::to_string`. The test shows the formatter and the reconstruction agree; it does not run the original firmware, so that the image prints the same bytes on a board is pending (device) |
| LP-5 | `BenchProtocol.PayloadOffPrintsTheLegacyLineForEveryKindAndHeader`, `PayloadOnKeepsELinesForKindsWithoutAPayloadLine`, `TheKindOfSampleSelectsTheLine`, `PayloadIsOffAtPowerUp`; `tests/bench/test_payload.py` | With payload off every event prints the E line of the reconstruction in LP-4; with it on a kind without a payload line still prints its E line, and the kind, not the sensor ID, selects the tag; payload is off at power-up; the harness turns payload off at session start |
| LP-6 | `BenchProtocol.RotationVectorLineIsQuaternionThenAccuracy`, `GameRotationLineHasNoAccuracyField`, `VectorLineIsXyz`, `UncalibratedLineIsValueThenBias`, `GyroIntegratedLineIsQuaternionThenAngularVelocity`, `FixedNumbersHaveFiveDigitsAndRoundToNearest`, `ANegativeValueThatRoundsToZeroCarriesNoSign`, `ANonFiniteValueIsNan`; payload lines on hardware (device, pending) | Each line has the fields, order and number format stated; the frame and rotation-direction statements are not tested and the on-device print is a device run, not yet made |
| LP-7 | `BenchProtocol.SaveDcdResponseReportsItsStatus`, `MeCalibrationResponseReportsEachRoutine`, `OtherResponsesPrintTheirRawResults`, `EachRequestAwaitsTheResponseOfItsCommand`, `NothingIsAwaitedAtPowerUp`, `OnlyTheAwaitedResponseIsTakenAndOnlyOnce`, `ATimeoutEndsTheWait`, `ARefusalEndsTheWaitExceptBusy`, `AClearEndsTheWait`; review of `handle_calibration_command`, `handle_restart` and the main loop in `examples/bench/main.cpp`; command responses on hardware (device, pending) | Each response prints as the line stated; each request waits for the response of its command, nothing is awaited at power-up, a response is taken once and only if awaited, and a timeout, a refusal other than `busy` and a clear end the wait; that `reset`, `cleardcd` and `boot` call the clear and that the main loop passes the session's timeout count is review, and the hub's responses are a device run, not yet made |
| LP-8 | `BenchProtocol.WorstEnabledSetFitsHalfTheFullSpeedBulkCeiling` | The longest line of each of thirteen sensors, at the rates of LP-8, totals 381140 bytes a second, at most half of 1216000; the rates of the uncalibrated gyroscope and magnetometer are UNCONFIRMED; sustained USB throughput on the board is not tested |
