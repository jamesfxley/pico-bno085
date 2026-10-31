# Future work

Work deliberately not done, one entry per deferral, in the form `EN-03.7`
defines.

## FW-1 — RP2350 only; plain Pico 2 unvalidated

### What is absent

The HAL builds for RP2350 boards only, and the driver declares
`PLATFORM rp2350`. No RP2040 HAL exists. Running on a plain Pico 2 is
unvalidated: the bench runs on a Pico 2 W, and the specification marks a plain
Pico 2 `UNCONFIRMED`.

### Why it is not built

pico-logging's sink and level atomics need the RP2350's exclusive access, so
the driver depends on a library that does not build for the RP2040. The
RP2040's pin-mux residues are unconfirmed until read from its datasheet (the
RP2350's are confirmed and checked in code), and a plain Pico 2 is unvalidated
because the only bench board is a Pico 2 W.

### What would have to be true to build it

An RP2040 HAL over the SDK's I2C, SPI and GPIO interfaces, which are common to
both chips; the RP2040 pin-mux residues confirmed from the datasheet; a bench
run on a plain Pico 2; and, for the RP2040, pico-logging building for it
(pico-logging:FW-1). The pin-mux predicates (`detail::pin_mux`) encode the
RP2350's rules, so an RP2040 HAL needs its own residues beside them.

### Version cost

Not breaking under `EN-06.1`: it adds a platform. Whether an RP2040 HAL needs a
change to a public HAL header is UNCONFIRMED.

### Affected repositories

pico-bno085; pico-logging:FW-1.

## FW-2 — Hub behaviour UNCONFIRMED on device, including BNO086

### What is absent

No bench observation is recorded for behaviour the CEVA documents leave open:
whether a continuation issued before the hub reasserts HINTN complies with the
SHTP reference manual (1000-3535 §2.5), whether the hub truncates or rounds the timestamp tick, whether the
hub re-takes a cargo's base delta against a retried assertion, which interrupt
a fused report is timed from and whether it shares a sample phase with the
gyroscope, whether calibration survives `reset()`, and whether a BNO086 behaves
as the driver assumes. No BNO086 has been run on the bench; the driver tolerates its
zero-length packets, but support for the part is UNCONFIRMED.

### Why it is not built

Only an observation on a real part can close each item. The documents are
silent, and the driver follows the stated fallback and marks the point.

### What would have to be true to build it

Each item closed by a recorded bench observation, or left `UNCONFIRMED` with
the reason it cannot be observed. A BNO086 on the bench is needed for the last.

### Version cost

Not breaking under `EN-06.1`: an observation adds specification wording and
bench records.

### Affected repositories

pico-bno085; pico-sh2:FW-5, which holds the protocol-layer partner.

## FW-3 — `service()` cross-core guarantee

### What is absent

`service()` is called from one context. Calling it from the other core is
neither supported nor prevented, and no guarantee is stated.

### Why it is not built

The seqlock publication of the HINTN timestamp uses full thread fences, so it is
probably correct for a reader on the other core. Nothing around it has been
reviewed for cross-core use, and no test covers it. Claiming support from the
fences alone would claim a guarantee from the one part that was examined.

### What would have to be true to build it

The transports, `EdgeLatch`, `HintnLatch`, `SplitReadCompensator` and the
re-apply pass audited for cross-core use, with the single-caller assumption
either held by construction or documented as a requirement on the consumer; and
a test that exercises two cores on hardware.

### Version cost

Depends. Not breaking under `EN-06.1` if it lands as a documented guarantee
with no signature change; breaking if the transports' interface must carry a
synchronisation concept.

### Affected repositories

pico-bno085.

## FW-4 — Two hubs on one board

### What is absent

The interrupt dispatch table holds two latches and each transport owns its own
pins, so two hubs would work electrically. The driver façade assumes one
session, and no run drives two parts.

### Why it is not built

Untested: nothing has been exercised with two parts attached, and the façade
was written for the case that exists.

### What would have to be true to build it

Either the façade carries more than one session, or a consumer holds one façade
per hub with the dispatch table shared safely between them. Two parts on one
board, and a bench run that drives both at once, since the failures that matter
are in the interrupt path where the sharing is.

### Version cost

Depends. Not breaking under `EN-06.1` if a consumer holds one façade per hub
and the public surface is unchanged; breaking if the façade itself gains a hub
concept.

### Affected repositories

pico-bno085.

## FW-5 — UART modes

### What is absent

The BNO08x's UART-SHTP and UART-RVC modes are not supported. The PS0 and PS1
combinations that select them are out of scope for the driver.

### Why it is not built

Support follows a change to pico-sh2's transport seam (pico-sh2:FW-3), which
today expresses readiness only for a bus with a data-ready signal. A UART
transport here cannot be written before that seam can carry it.

### What would have to be true to build it

pico-sh2:FW-3 done, and a UART transport and its bring-up written against the
new seam.

### Version cost

Breaking under `EN-06.1`: it follows a change to the seam that this driver
implements.

### Affected repositories

pico-bno085; pico-sh2:FW-3.

## FW-6 — Decoder captures from the bench

### What is absent

No reports are captured from the part for the sensors beyond the core set
(significant motion, shake, flip, pickup, sleep, tilt, pocket, circle, heart
rate and personal activity), so pico-sh2 has no real golden vectors for them.

### Why it is not built

None has been exercised against the part. A decoder whose golden vector is
written from the same reading of the manual as the decoder cannot show the
decoder is right.

### What would have to be true to build it

Each sensor enabled on hardware through the bench application and its reports
captured, for use as golden vectors in pico-sh2 (pico-sh2:FW-2).

### Version cost

Not breaking under `EN-06.1`: it adds bench captures and, in pico-sh2,
decoders.

### Affected repositories

pico-bno085; pico-sh2:FW-2, where the decoder table and vectors land.
