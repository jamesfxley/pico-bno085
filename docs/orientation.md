# pico-bno085

This document covers local build settings and implementation invariants.
The [scope table](technical-specification.md#1-scope-and-targets) names the
supported platform and validation gaps (FW-1, FW-2). The driver implements
pico-sh2's `IHalTransport` interface for the RP2350.

Engineering rules are in the
[pinned standard](../infra/house/standard/README.md). The
[family routes](../infra/agents/AGENTS.md) name the guides.

## Commands

The [family build guide](../infra/docs/build.md) owns shared build, format,
lint and gate commands. `python3 dev.py check --ci` runs offline checks;
`--list` names them. `python3 dev.py gate consumer` checks relocated installed
packages.

Firmware requires `PICO_SDK_PATH`. `HOUSE_MATRIX_BOARD` chooses the board.
`HOUSE_MATRIX_SANITIZE=ON` enables the sanitizer tree.
`HOUSE_MATRIX_PROFILE` selects `PICO_BNO085_PROFILE` (`default` or `large`),
which forwards to pico-sh2. Both profiles have host tests, installed consumers
and firmware footprint checks.

The randomised host `Soak` test runs under CTest.
`PICO_BNO085_SOAK_ITERATIONS` selects its length. The weekly job uses 500000
iterations before online checks. Fuzz cases run bounded input counts under
CTest.

Dependencies use explicit pins. Source overrides are explicit and reported;
sibling checkouts are not discovered. Device validation is separate from build
and footprint checks. Device commands and their authorisation are covered by
[the device guide](../infra/guides/device.md). The
[benchmark guide](../examples/bench/README.md) owns bus selection, line protocol
and baseline promotion.

## Layout

- `src/driver/` contains the portable driver.
- `src/hal/` contains host-tested logic. `src/hal/rp2350/` contains SDK
  adapters.
- `src/testing/` contains the installed tracing decorator.
- `tests/<module>/` mirrors `src/` (CXX-13.2). Test lint overrides are in
  `tests/.clang-tidy` and per-module files.
- `examples/` contains the minimal image and device benchmark.
- `docs/README.md` routes readers to specifications and guides.

The [architecture](architecture.md) owns module design and export descriptions.

## Conventions

### House policy

Common rules follow the
[pinned engineering standard](../infra/house/standard/README.md).

### Repo specifics

- Host code takes `-Werror` through `pico_bno085_strict`. Firmware applies it
  to owned sources through `house_cxx_werror_sources`, because SDK INTERFACE
  libraries add their own sources to the same target.
- `house_cxx_enable_sanitizers` must precede `house_cxx_enable_gtest`.
  Directory compile options reach only subsequently created targets, so the
  reverse order leaves GoogleTest uninstrumented. House rejects that order.
  Partial ASan container instrumentation can report false overflows.
- `read_transfer` must return success for an empty or absent hub.
  An idle-bus error would fault startup or spuriously restart the handshake
  in READY. The [architecture](architecture.md#the-two-transports) explains
  recovery.
- HINTN timestamp publication follows the interrupt-writer seqlock strategy
  described in the [architecture](architecture.md#hal-modules) (AR-2).
- `data_ready()` reads live HINTN. A read without a latched edge is a level
  fallback and can use an earlier edge. `level_fallback_reads` counts an upper
  bound on those timestamp cases, also reported by bench `stat` (HR-1).
- Each transfer carries the edge taken when `consume()` clears the latch.
  Taking the edge earlier could use the previous assertion while counting
  the read as edged (AR-2).
- Protocol-select pins change with NRST asserted. The wiring supports both
  buses without jumper changes (HR-3).
- Queue-full enables return `DEFERRED` and schedule reapplication. Queue-full
  disables return `NO_ROOM` and retain their desired entry (BR-2).
- Reapplication after reset resumes across `service()` calls using per-entry
  flags because the sensor table can exceed queue depth (AR-3).
- The façade returns `pico_bno085::Error`. Transports implement the pico-sh2
  contract and return `pico_sh2::Sh2Error` (AR-4). The specification owns the
  exhaustive mapping requirement. A retained SPI duplex cargo is ready with
  its saved edge and is returned without bus activity (HR-4).
