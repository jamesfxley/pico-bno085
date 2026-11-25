# Future work

This document records deferred work in the form EN-03.7 defines.

## FW-1 — RP2350 only; plain Pico 2 unvalidated

### What is absent

The HAL declares `PLATFORM rp2350` and has no RP2040 implementation.
A plain Pico 2 is UNCONFIRMED. The benchmark runs on a Pico 2 W.

### Why it is not built

pico-logging's atomics need RP2350 exclusive access and do not build for
RP2040. The RP2040 pin mapping is unconfirmed. Only a Pico 2 W is available
for the benchmark.

### What would have to be true to build it

RP2040 support needs an SDK-backed HAL, datasheet-confirmed pin-mux
predicates beside `detail::pin_mux`, and pico-logging support
(pico-logging:FW-1). Validation of a plain Pico 2 needs a benchmark run on it.

### Version cost

Adding a platform is not breaking under EN-06.1. Whether an RP2040 HAL needs
a public header change is UNCONFIRMED.

### Affected repositories

pico-bno085; pico-logging:FW-1.

## FW-2 — Hub behaviour UNCONFIRMED on device, including BNO086

### What is absent

The following hub behaviours lack device evidence and remain UNCONFIRMED:

- A continuation before HINTN reassertion (1000-3535 §2.5).
- Timestamp tick truncation or rounding.
- Recalculation of the base delta after a retried assertion.
- The interrupt reference for fused reports and their sample phase relative
  to the gyroscope.
- Calibration survival across `reset()`.
- The layout of the tare response, which 1000-3625 §6.4.3 names but does not
  give, and whether the get-calibration command answers with the layout of
  §6.4.6.3 (LP-7).
- The direction of the rotation a quaternion expresses, the axes of the
  reference frame, and the reference the gyro-integrated rotation vector takes
  by default (LP-6).
- BNO086 support. The driver tolerates its zero-length packets, but no BNO086
  has been tested.

### Why it is not built

The CEVA documents leave these behaviours open. The driver marks the gaps
and uses its documented fallbacks. Each needs observation on a real part.

### What would have to be true to build it

Each item needs a recorded observation or an UNCONFIRMED result explaining
why it cannot be observed. BNO086 validation needs that part on the bench.

### Version cost

An observation adds specification wording and run records without a breaking
change (EN-06.1).

### Affected repositories

pico-bno085; pico-sh2:FW-5, which holds the protocol-layer partner.

## FW-3 — `service()` cross-core guarantee

### What is absent

`service()` is called from one context. Calling it from the other core is
neither supported nor prevented, and no guarantee is stated.

### Why it is not built

Timestamp publication uses full thread fences and is probably safe across
cores. The surrounding code has neither a cross-core review nor a test, so
the fences do not establish support.

### What would have to be true to build it

Support needs a cross-core audit of both transports, `EdgeLatch`,
`HintnLatch`, `SplitReadCompensator` and sensor reapplication. The single-caller
assumption needs enforcement or documentation as a consumer requirement.
A device test needs to exercise both cores.

### Version cost

A guarantee without a signature change is not breaking under EN-06.1.
A transport interface that must carry a synchronisation concept is breaking.

### Affected repositories

pico-bno085.

## FW-4 — Two hubs on one board

### What is absent

The dispatch table has two latch slots, and each transport owns its pins.
The façade holds one session. No run has driven two parts.

### Why it is not built

Two-part operation is untested. The façade assumes one hub.

### What would have to be true to build it

Support needs either several sessions per façade or one façade per hub,
with safe dispatch sharing. A benchmark needs to drive two parts on one board
to exercise the shared interrupt path.

### Version cost

One façade per hub with an unchanged public API is not breaking under
EN-06.1. Adding a hub concept to the façade is breaking.

### Affected repositories

pico-bno085.

## FW-5 — UART modes

### What is absent

The BNO08x's UART-SHTP and UART-RVC modes are not supported. The PS0 and PS1
combinations that select them are out of scope for the driver.

### Why it is not built

pico-sh2's transport interface models readiness through a data-ready signal.
UART support depends on changing that interface (pico-sh2:FW-3).

### What would have to be true to build it

pico-sh2 UART support needs to be complete before this driver can add a UART
transport and startup sequence (pico-sh2:FW-3).

### Version cost

The transport interface change is breaking (EN-06.1).

### Affected repositories

pico-bno085; pico-sh2:FW-3.

## FW-6 — Decoder captures from the bench

### What is absent

No reports are captured from the part for the sensors beyond the core set
(significant motion, shake, flip, pickup, sleep, tilt, pocket, circle, heart
rate and personal activity), so pico-sh2 has no real golden vectors for them.

### Why it is not built

These decoders have no device evidence. Test vectors derived from the same
manual reading as a decoder can reproduce its mistakes.

### What would have to be true to build it

Each sensor needs a benchmark capture to supply pico-sh2's expected report
bytes (pico-sh2:FW-2).

### Version cost

Adding captures and pico-sh2 decoders is not breaking (EN-06.1).

### Affected repositories

pico-bno085; pico-sh2:FW-2, where the decoder table and vectors land.

## FW-7 — Bench USB throughput with payload lines

### What is absent

No run has printed payload lines on the board, so the rate its USB stack
sustains for them, and what a full link does to `service()`, are not measured.
LP-8 bounds the line rate against the bus's ceiling only.

### Why it is not built

A measurement needs an authorised device session. USB 2.0 is not among the
local references, so the ceiling LP-8 uses is also unchecked against its
source.

### What would have to be true to build it

A device run enables the largest set the bench can run with payload on, counts
lines against the rate the hub grants, and records the sequence gaps and the
`service_latency_max_us` it reports. LP-8 is then restated against the figure
it finds.

### Version cost

A measurement and specification wording are not breaking (EN-06.1).

### Affected repositories

pico-bno085.
