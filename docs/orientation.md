# pico-bno085

C++20 driver for the Bosch/CEVA **BNO085**/BNO086 IMU on RP2350 boards (the
specification's Hardware target row says which are validated). Owns the board-specific pins, reset, and HINTN interrupt
handling over I2C or SPI, and presents a callback API for enabling sensors and
receiving typed events. The SH-2/SHTP protocol layer lives in the sibling
library pico-sh2; this repo implements its `IHalTransport` seam.

Orientation for anyone working in this repository: how to build and gate it,
how it is laid out, and the repository's invariants and traps.

Engineering standards, cited by rule ID (`EN-`/`CXX-`):
<https://github.com/jamesfxley/engineering-standard>

## Commands

`python3 dev.py check --ci` runs the offline gates; `--list` names them.
`python3 dev.py build` configures, clean-builds and tests enabled trees.
`python3 dev.py build --tree host` selects the host tree and
`python3 dev.py gate consumer` verifies relocated installed packages.
`python3 dev.py format --check` and `python3 dev.py lint` run the tool groups.
Read `infra/agents/AGENTS.md` for the pinned engineering, C++ and family guides.

Firmware needs PICO_SDK_PATH; HOUSE_MATRIX_BOARD chooses the board.
HOUSE_MATRIX_SANITIZE=ON enables the sanitizer tree. HOUSE_MATRIX_PROFILE maps
to PICO_BNO085_PROFILE (`default` or `large`), which forwards to pico-sh2.
Both profiles have host tests, installed consumers and firmware footprint gates.
The randomized host Soak test runs under CTest; PICO_BNO085_SOAK_ITERATIONS
selects a longer run. The weekly job prepares the existing host Soak case
with 500000 iterations before the online checks.
Fuzz targets run bounded input counts through CTest.

Product dependencies are explicit pins. A source override is explicit and
reported; sibling checkouts are not discovered. Device validation is separate
from build/static-footprint evidence. Follow the pinned family device guide and
examples/bench/README.md before an authorized device run. The benchmark keeps
its bus selection, line protocol and baseline promotion behavior.

## Layout

- `src/driver/` — the platform-independent `Bno085` façade over a pico-sh2
  session: sensor enable, calibration/tare, stats. Host-tested.
- `src/hal/` — `pico_bno085::hal_core`: the SDK-free, host-tested half of the
  HAL (`SplitReadCompensator`, `EdgeLatch`, `detail::strap_plan`).
- `src/hal/rp2350/` — `pico_bno085::hal`: firmware-only RP2350 transports
  (`I2cHalTransport`, `SpiHalTransport`), `control_pins`, and the `HintnLatch`
  interrupt latch. A module of its own because `CXX-13.1` puts one target in
  one directory; the include spelling stays `pico_bno085/hal/`.
- `src/testing/` — `TracingTransport`. Test and diagnostic support, in the
  install export set alongside the driver: a downstream project testing against
  this seam links the same decorator this repository's tests and bench image
  use. `tests/consumer/` links `pico_bno085::testing`, so an export set that
  drops it fails there.
- `tests/<module>/` — the test tree, mirroring `src/` (`CXX-13.2`), so `src/` is
  exactly the shippable surface and no install, lint or coverage rule over it
  needs a test exclusion. `tests/.clang-tidy` is the library-owned registered overlay; a per-module
  config beside it adds only what that module needs.
- `examples/` — `minimal/` (I2C bring-up) and `bench/` (validation app,
  `bench.py`, baselines).
- `docs/` — normative spec (requirement IDs `AR/HR/PR/BR/TR/OR`) and the
  `usage-guide.md` wiring guide. The BNO08x datasheet is cited by document
  number rather than redistributed; `docs/README.md` says where to get it.

## Conventions

### House policy

The [pinned engineering standard](../infra/house/standard/README.md) owns the
common rules. The [family routes](../infra/agents/AGENTS.md) name the guides.

### Repo specifics

- **`-Werror` follows the toolchain.** The host tree links no SDK sources, so it
  sits on `pico_bno085_strict`; a firmware target cannot take it, because the
  SDK compiles its own sources into whichever target links its INTERFACE
  libraries, so this repo's firmware translation units take it per source via
  `house_cxx_werror_sources`. `src/driver/` is built both ways and registers
  its source only in the firmware branch.
- **`house_cxx_enable_sanitizers` must precede `house_cxx_enable_gtest`.**
  `add_compile_options` reaches only targets created after it, so the reverse
  order leaves the fetched GoogleTest uninstrumented while this repo's code
  carries ASan. House refuses that order outright.
- **A scope config restates pico-sh2's exemptions on purpose.** A header's
  diagnostics follow the configuration of the translation unit that includes it,
  not the config beside the header, so pico-sh2's own `src/.clang-tidy` does not
  apply here. Both `src/.clang-tidy` and `examples/.clang-tidy` therefore carry
  those rules.
- **`read_transfer` must return success, not error, for an empty or dead bus**.
  `Session::drain_transfers()` loops while
  `data_ready()`, bounded per `service()`; a read error faults the session
  during startup and in READY restarts the handshake, counted in
  `transport_restarts` — so an error for an idle bus costs a startup fault or a
  spurious handshake restart.
- **The HINTN edge timestamp is published under a seqlock**, which is
  `pico_util::Published`'s, not this repository's. The ISR writes a 64-bit
  value as two 32-bit stores on the M33, so an unprotected read from
  `service()` context can tear. The strategy is named at the declaration
  because the opposite preemption direction needs the opposite protocol and a
  host test cannot tell them apart (AR-2).
- **`data_ready()` reads the live pin level, not the latch** (HR-1). The hub
  holds HINTN asserted while a cargo waits, so a missed or coalesced edge
  cannot wedge `service()`, and an edge that outlives its assertion cannot
  start a read: on a BNO085 over I2C, a read with no cargo waiting is
  clock-stretched until one arrives or the transfer timeout ends it. A read with no
  latched edge behind it, a level fallback, may anchor
  its events to an *earlier* timestamp; `HintnLatch::consume()` counts these as
  `level_fallback_reads`, an upper bound on such reads, and the bench `stat`
  line carries the total.
- **A read's anchor comes from the `consume()` that clears the latch** (AR-2),
  and the transport returns it with the transfer. The line can be seen asserted
  before the handler has latched its edge, so an anchor read any earlier is the
  previous edge's: the cargo is timed early by the gap back to the previous
  assertion, on a read counted as edged.
- **Protocol-select straps are driven with the hub held in reset**, before the
  session releases NRST (HR-3). The same wiring supports both buses with no
  jumper moved.
- Queue-full is recoverable (BR-2): a refused enable schedules a re-apply, a
  refused disable keeps its desired entry.
- Sensor re-enable after a hub reset (AR-3) runs from a cursor that resumes
  across `service()` calls, because the TX queue is shallower than the sensor
  table.
- **The façade answers in `pico_bno085::Error`, the transports in
  `pico_sh2::Sh2Error`** (AR-4). `from_sh2()` maps at the boundary, collapsing
  the four framing codes onto `PROTOCOL` and both capacity codes onto
  `NO_ROOM`. It names every enumerator and carries no `default:`, so an
  enumerator a later pico-sh2 appends is a `-Wswitch` error under `-Werror`
  rather than a silent remap: taking a new pico-sh2 is a build failure that
  names the code, which is the guarantee. The trailing `return` keeps the
  function well-formed for a value outside the enumeration and is unreachable
  from a correctly built tree. The transports are not an exception to
  it: they implement `pico_sh2::IHalTransport`, so its taxonomy is their
  contract.
  A SPI duplex cargo already in staging is also ready; returning it uses its
  saved edge and performs no bus activity (HR-4).
