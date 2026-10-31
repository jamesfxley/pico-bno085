# pico-bno085

[![CI](https://github.com/jamesfxley/pico-bno085/actions/workflows/ci.yml/badge.svg)](https://github.com/jamesfxley/pico-bno085/actions/workflows/ci.yml)

`pico-bno085` is a C++20 driver for the Bosch/CEVA **BNO085** IMU on RP2350
boards (FW-1), built and benched on a Pico 2 W. The pin-compatible **BNO086** is
not validated: its support is UNCONFIRMED (FW-2). It drives the sensor over
I2C or SPI, owns the board-specific pins, reset, and HINTN interrupt handling,
and presents a small callback-based API for enabling sensors and receiving
typed, unit-converted events.

The SH-2 protocol and its SHTP transport live in a separate, platform-independent
library, [pico-sh2](https://github.com/jamesfxley/pico-sh2); this repository
implements that library's `IHalTransport` seam over the RP2350 I2C and SPI
peripherals and adds device policy, calibration/tare, and diagnostics. Because
the protocol layer is host-tested against a fake hub, only the transports here
need real hardware.

## Quick start

```cmake
include(FetchContent)
FetchContent_Declare(pico_bno085
    GIT_REPOSITORY https://github.com/jamesfxley/pico-bno085.git
    GIT_TAG v0.1.0)  # pin the release you build against
FetchContent_MakeAvailable(pico_bno085)

target_link_libraries(app PRIVATE pico_bno085::core)
```

```cpp
#include <pico_bno085/pico_bno085.hpp>
```

All public symbols live in `namespace pico_bno085` with headers under the
`pico_bno085/` prefix.

## Layout

- `src/driver/` — the platform-independent `Bno085` façade over a pico-sh2
  session: sensor enable, calibration/tare, stats. Host-tested.
- `src/hal/` — `pico_bno085::hal_core`: the parts of the HAL that link no SDK
  and are host-tested — the split-read compensation, the interrupt edge latch
  and the strap order.
- `src/hal/rp2350/` — `pico_bno085::hal`: the transports themselves, over the
  I2C, SPI, GPIO and timer peripherals. Firmware-only.
- `src/testing/` — `TracingTransport`, the transfer tracer. In the install
  export set as `pico_bno085::testing`, for downstream driver tests and
  `pico_sh2::testing`: a downstream project testing against this seam links the
  same decorator this repository's tests and bench image use.
- `examples/` — `minimal/` (the smallest thing that streams) and `bench/` (the
  on-device validation harness, `bench.py`, and the baselines).
- `docs/` — the normative specification (requirement IDs `AR/HR/PR/BR/TR/OR`),
  [architecture](docs/architecture.md) and the
  [wiring guide](docs/usage-guide.md). The BNO08x datasheet every electrical
  claim rests on is cited by document number rather than redistributed.

## Building & tests

Initialize the pinned family, House and standard with
`git submodule update --init --recursive`. Product pins live in CMakeLists.txt;
House imports the family's gates, firmware tree and defaults through house.toml.

```sh
python3 dev.py check --ci
python3 dev.py build --tree host
python3 dev.py gate consumer
```

The host suite tests the portable driver against pico-sh2's FakeHub. Firmware
builds the RP2350 transports and both example images, with PICO_SDK_PATH set.
HOUSE_MATRIX_PROFILE=large selects the wider profile forwarded to pico-sh2.
[Orientation](docs/orientation.md) lists local inputs and invariants; the
[family build guide](infra/docs/build.md) owns shared commands. Source overrides
are explicit; sibling checkouts are not discovered. Device runs are separate.

## Wiring

See [docs/usage-guide.md](docs/usage-guide.md) for the recommended one-wiring-for-both-buses
pinout. The protocol-select pins are driven by firmware, so the same board
supports I2C and SPI with no jumper moved.

## Documentation

- [Documentation index](docs/README.md) — what each document is for
- [Technical specification](docs/technical-specification.md) — normative;
  requirement IDs `AR/HR/PR/BR/TR/OR` with their pass criteria
- [Architecture](docs/architecture.md) — how the driver is layered over
  pico-sh2, and where a change belongs
- [Usage guide](docs/usage-guide.md) — wiring, bus selection, first run on a
  board
- [Future work](docs/future-work.md) — what is deliberately not built, and why
- [Standing deviations](docs/deviations.md) — scope-wide rule suppressions and
  their review dates
- [Orientation](docs/orientation.md) — how the repository is laid out, and the
  traps it has already paid for
- [Contributing](CONTRIBUTING.md) — how a change is made and when it is done
- [Releases](https://github.com/jamesfxley/pico-bno085/releases) — what changed
  per release, and what to change in your code when an API broke

Measured results are not in `docs/`. Device runs and the conditions they were
captured under live with the benchmark:
[examples/bench/baselines/](examples/bench/baselines/).

## Releases

Versioning is semver (`EN-06.1`): a breaking change lands at a minor version
bump while the major version is 0, and at a major version bump from 1.0. The
notes on each [release](https://github.com/jamesfxley/pico-bno085/releases)
record what changed and, for a break, what to change in your own code
(`EN-06.3`).

## AI-assisted development

This project is developed with AI assistance. Design decisions, review and
acceptance are human; an AI agent writes and refactors code under direction and
against a written engineering standard. Every change passes the repository's own
gates — build, tests, formatter, linter, deviation counts — before it lands.

This section is disclosure, not a warranty. The terms in [LICENSE](LICENSE)
apply unchanged, including its disclaimer of warranties.
