# pico-bno085 Architecture

How the driver is put together. The normative requirements live in
[technical-specification.md](technical-specification.md); the wiring and
bring-up procedure in [usage-guide.md](usage-guide.md).

## What is here and what is not

The protocol — SHTP framing, the SH-2 session, report decoding — is
[pico-sh2](https://github.com/jamesfxley/pico-sh2)'s, and it is
platform-independent. This repository is what makes that run on an RP2350
board: the pins, the reset and strap sequencing, the interrupt, the two buses,
and the policy the protocol layer deliberately leaves to a consumer.

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

## The HAL, split by what needs hardware

The HAL is two modules, and the division is not cosmetic. Protocol reasoning
tangled up with SDK calls is testable only by flashing a board, so the half that
needs no hardware is separated out and host-tested. `CXX-13.1` puts one target
in one directory, so the split is visible in the tree rather than only in a
`CMakeLists.txt`.

**`pico_bno085::hal_core`** (`src/hal/`) is header-only, links no SDK, and
builds on host:

- **`SplitReadCompensator`** undoes what the host's own read pattern does to a
  cargo. Retrieving one over I2C takes a header peek and then a read of the
  whole cargo, and the hub answers that split with a continuation flag and an
  extra sequence number that describe the *reads*, not the cargo. Getting this
  wrong makes every cargo look like a dropped one.
- **`EdgeLatch`** publishes the HINTN edge and its timestamp from the interrupt
  to `service()` through a seqlock, because a 64-bit store is two instructions
  on the M33 and a reader can land between them. The protocol is
  `pico_util::Published`'s `SEQLOCK_WRITER_NOT_PREEMPTIBLE` strategy, named
  rather than defaulted: the writer is the handler and the reader is mainline,
  so a reader that spins always terminates. What stays here is the latch policy
  — the consumable flag, the level-fallback count, and the edge each read is
  anchored to (AR-2).
- **`detail::strap_plan`** is the order the strap and reset lines are driven
  in (HR-3), as data: NRST asserted first, then PS1, PS0 and BOOTN, and SA0
  only for I2C. `ControlPins::init()` drives those steps and nothing else, so
  the order is tested on the host rather than only on a wired board.
- **`detail::pin_mux`** is the RP2350 function-mux rules (datasheet Table 677)
  as predicates: which GPIO can carry I2C SDA or SCL, or SPI RX, SCK or TX, for
  a given instance and bank size. Each transport's `init()` refuses a pin they
  reject, before any pin is touched, and `pins.hpp` binds its default pins to
  them by `static_assert`.

**`pico_bno085::hal`** (`src/hal/rp2350/`) is firmware-only and holds what
genuinely needs the SDK: the I2C and SPI peripherals, GPIO, and the timer. It
nests inside `src/hal/` and exports the same `pico_bno085/hal/` include
spelling, so the directory layout is invisible to a consumer (`CXX-13.3`).

`TransportCore` sits between them, holding what both buses need regardless of
how bytes move — the strap and reset lines, the HINTN latch, the pin
configuration, the monotonic clock, and the counters. Both transports answer
`counters()` with the same type, so a consumer reading them needs no knowledge
of which bus it is on.

## The testing target

**`pico_bno085::testing`** is header-only, built for host and firmware, and
exported with the package:

- **`TracingTransport`** decorates any transport and reports the raw bytes of
  every transfer to a sink. The session talks to the hub through one seam, so
  that seam is the only place the whole conversation is visible. Its callers are
  this repository's tests, the bench example, and any downstream project testing
  against the same seam — which is why it ships rather than staying in-tree.

## The two transports

They differ in how they move bytes and in what they can report:

|  | I2C | SPI |
|---|---|---|
| Retrieval | Header peek, then the cargo (needs the split-read compensation) | Header and body inside one CSN assertion |
| Hub absent | The address is not acknowledged | The header reads back as the reserved dead-bus pattern |
| Host-initiated write | Just write | WAKE, wait for HINTN, then CSN, then drop the wake assertion (HR-1) |
| Idle poll | A BNO086 answers a read with nothing to send with a null header; the continuation's second HINTN assertion is discarded, so it opens no service-latency span and backs no later read as edged (HR-5, HR-6) | Nothing extra |

Both report an absent read the same way, and neither reports it as an error: a
read is only issued behind a HINTN assertion, so a hub that does not answer has
left the bus since asserting — it is resetting, not broken, and the session's
reset recovery handles it.

## Policy the façade owns

`Session` holds no record of what was asked for, on purpose: it implements the
protocol, and what to do after a hub reset is a decision, not a protocol rule.
`Bno085` makes that decision.

It keeps a **desired-sensor table**, and re-applies every entry when the
session reaches READY — after startup and after any hub reset. The table can
hold more sensors than the transmit queue has slots, so a pass can span several
`service()` calls. Each entry carries a pending flag, set when a pass is
scheduled and cleared when the entry is sent, and the next call resumes with
the entries still pending. The flag belongs to the entry rather than to a
position in the table, so an `enable_sensor()` or `disable_sensor()` between
calls cannot drop an unsent entry from the pass. The re-apply is keyed on a
reset being outstanding rather than only on the edge into READY: a reset whose
whole re-handshake drains inside one `service()` shows no edge, and keying on
the edge alone would leave the hub streaming nothing while the table said
otherwise.

`enable_sensor` distinguishes **applied from deferred**, because "recorded, and
the hub will hear about it on a later pass" is an ordinary outcome — the
session is not READY yet, or the queue is momentarily full — and a caller
waiting to see events needs to tell it from "sent".

## Boundaries the façade keeps

`Stats` holds only the counters the driver owns. The session's are read through
`counters()` and the transport's through the transport, rather than being
copied into a struct here — copying them would make pico-sh2's counter list
part of this driver's API, so adding a counter there would break consumers
here.

The façade is non-copyable and non-movable, as are `HintnLatch` and
`TransportCore`: the session's sinks are bound to the façade and the interrupt
dispatch table points at the latch, so a copy would leave either pointing at
the original.

## Allocation

Nothing allocates after `begin()`. The desired table is sized from the profile,
which is forwarded to pico-sh2 so the driver can never desire more sensors than
the session can hold. `pico_util::HeapWatermark` measures growth from a baseline
the bench arms once the session is constructed, which is what `b-heap` gates on.

## Wiring other than the reference board

`pins.hpp` describes one board. A consumer on different wiring sets the
control lines (HINTN, NRST, PS0, PS1, BOOTN, SA0) in `ControlConfig`, and the
bus pins and peripheral instance in its transport's own `Config`.

The `static_assert`s in `pins.hpp` bind its own I2C and SPI defaults to the
instance their pad numbers can reach — the RP2350's mux fixes each instance to
pin numbers of a particular residue. A transport's `Config` pins are checked at
run time by `init()` against the same rules (`detail::pin_mux`), which refuses
a pin the selected peripheral cannot reach or a GPIO outside bank 0 with
`INVALID_ARGUMENT`.

## Selecting a bus, and why it is not hot-switching

The bench selects a bus at run time, and switching tears the current transport
down — releasing its pads — and re-straps with the hub held in reset. That
works because the straps are only read at the reset edge.

What it is not is hot-switching: the hub restarts, so anything it had learned
in the session is gone. Adequate for a harness comparing the two buses; not
something an application should do.
