# pico-bno085

[![CI](https://github.com/jamesfxley/pico-bno085/actions/workflows/ci.yml/badge.svg)](https://github.com/jamesfxley/pico-bno085/actions/workflows/ci.yml)

`pico-bno085` is a C++20 driver for the CEVA BNO085 sensor hub on RP2350
boards. It controls I2C or SPI, reset, protocol-select pins and the HINTN
interrupt. Its callback API enables sensors and delivers typed events in
converted units. The benchmark runs on a Pico 2 W. Other board and part
support is described in the [scope table](docs/technical-specification.md#1-scope-and-targets).

[pico-sh2](https://github.com/jamesfxley/pico-sh2) implements SH-2 and its
Sensor Hub Transport Protocol (SHTP). This library implements pico-sh2's
`IHalTransport` interface and adds calibration, tare and diagnostics. Portable
logic is tested against a fake hub; transports also need device validation.

## Quick start

```cmake
include(FetchContent)
FetchContent_Declare(pico_bno085
    GIT_REPOSITORY https://github.com/jamesfxley/pico-bno085.git
    GIT_TAG v0.1.1)  # pin the release you build against
FetchContent_MakeAvailable(pico_bno085)

target_link_libraries(app PRIVATE pico_bno085::core)
```

```cpp
#include <pico_bno085/pico_bno085.hpp>
```

Public symbols use `namespace pico_bno085` and the `pico_bno085/` header prefix.

## Layout

- `src/driver/` contains the portable `Bno085` driver.
- `src/hal/` contains SDK-free HAL logic. `src/hal/rp2350/` contains the
  firmware transports.
- `src/testing/` contains the installed `TracingTransport` decorator for tests
  and diagnostics.
- `examples/minimal/` streams rotation vectors. `examples/bench/` contains the
  device benchmark, host script and run records.
- `docs/` contains the specification, architecture and usage guide.

The [architecture](docs/architecture.md) describes the modules and their public
targets. Internal exports are implementation details.

## Building & tests

Initialise the recursive pins with `git submodule update --init --recursive`.
Product dependency pins are in `CMakeLists.txt`. `house.toml` selects the family
build trees and checks.

```sh
python3 dev.py check --ci
python3 dev.py build --tree host
python3 dev.py gate consumer
```

Host tests use pico-sh2's `FakeHub`. Firmware builds both example images and
the SDK transports. [Orientation](docs/orientation.md#commands) lists the
required environment and profile options. The
[family build guide](infra/docs/build.md) owns shared commands.

Device runs are separate. `tests/device/` checks default wiring on both buses
and sensor restoration after reset, following the
[pinned device guide](infra/guides/device.md#device-tests).

## Wiring

The [usage guide](docs/usage-guide.md) describes wiring that supports both
buses. Firmware drives the protocol-select pins, so changing bus needs no
jumper change.

## Documentation

- [Documentation index](docs/README.md) routes readers to each document.
- [Technical specification](docs/technical-specification.md) defines requirements
  and pass criteria.
- [Architecture](docs/architecture.md) explains the modules and design reasons.
- [Usage guide](docs/usage-guide.md) covers wiring and startup on a board.
- [Future work](docs/future-work.md) records deferred work and evidence gaps.
- [Standing deviations](docs/deviations.md) records scope-wide suppressions.
- [Orientation](docs/orientation.md) covers local build settings and invariants.
- [Contributing](CONTRIBUTING.md) describes the change process.
- [Releases](https://github.com/jamesfxley/pico-bno085/releases) records changes
  and migration instructions.

Device measurements and their conditions are in
[examples/bench/baselines/](examples/bench/baselines/).

## Releases

Versioning follows [EN-06.1](infra/house/standard/engineering/06-release-and-versioning.md).
[Release notes](https://github.com/jamesfxley/pico-bno085/releases) follow
[EN-06.3](infra/house/standard/engineering/06-release-and-versioning.md).

## AI-assisted development

This project is developed with AI assistance. People direct design, review and
acceptance. An AI agent writes and refactors code against the written standard.
Changes pass the repository's build, test, format, lint and deviation checks
before merge.

The terms and warranty disclaimer in [LICENSE](LICENSE) apply.
