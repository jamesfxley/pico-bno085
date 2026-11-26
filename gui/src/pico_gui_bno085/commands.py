# SPDX-License-Identifier: MIT
"""Bench commands as records the controller can match to their replies.

Every builder validates its arguments the way the firmware's parser does and
raises `CommandError` before any line is built, so a refusal happens on the
host and nothing is sent. The bounds are the parser's: an unsigned 32-bit
decimal for `run` and the interval of `cfg`, two hexadecimal digits for a
sensor, and the fixed sets the `tare` parser accepts.
"""

from dataclasses import dataclass

from pico_gui.commands import CommandError

# The firmware parses each numeric argument with an unsigned 32-bit
# reader, so a value above this is refused with `# err parse`.
UNSIGNED_MAX = 4_294_967_295
_SENSOR_MAX = 0xFF
_TARE_AXES = (0x7, 0x4)
_TARE_BASIS_MAX = 5

# LP-8: the maximum rate of each sensor the bench can enable, from the
# datasheet's rate table.
MAX_RATE_HZ = {
    0x2A: 1000,
    0x05: 400,
    0x28: 400,
    0x08: 400,
    0x29: 400,
    0x06: 400,
    0x04: 400,
    0x02: 400,
    0x07: 400,
    0x01: 500,
    0x03: 100,
    0x0F: 100,
    0x09: 90,
}

# LP-8 gives the uncalibrated gyroscope and magnetometer the rates of their
# calibrated counterparts and marks those two rates UNCONFIRMED.
UNCONFIRMED_RATES = frozenset({0x07, 0x0F})

# 1000-3625 §6.4.3.1: the axes bitmap and the rotation vector the tare is
# taken against (P1 and P2 of Figure 48).
TARE_AXES_NAMES = {
    0x7: "X, Y and Z",
    0x4: "Z alone",
}
TARE_BASIS_NAMES = {
    0: "Rotation vector",
    1: "Game rotation vector",
    2: "Geomagnetic rotation vector",
    3: "Gyro-integrated rotation vector",
    4: "ARVR-stabilised rotation vector",
    5: "ARVR-stabilised game rotation vector",
}

__all__ = (
    "MAX_RATE_HZ",
    "TARE_AXES_NAMES",
    "TARE_BASIS_NAMES",
    "UNCONFIRMED_RATES",
    "UNSIGNED_MAX",
    "DeviceCommand",
    "cal",
    "calget",
    "cfg",
    "connect_bus",
    "connect_payload",
    "off",
    "run",
    "tare",
)


@dataclass(frozen=True)
class DeviceCommand:
    """One command line and what its acceptance means to the session.

    `success` is the prefix of the reply that completes the command (LP-3).
    `needs_ready` marks the commands the driver refuses with `not_ready`
    until the session has reached READY (LP-2). `sensor`, `interval_us` and
    `enables` carry a `cfg` line's effect; `run` marks the line that keeps
    the stream flowing.
    """

    line: str
    success: str
    needs_ready: bool = False
    sensor: int | None = None
    interval_us: int | None = None
    enables: bool = False
    run: bool = False


def _hex_sensor(sensor: int) -> str:
    if not 0 <= sensor <= _SENSOR_MAX:
        raise CommandError(
            f"the sensor id {sensor} is not a byte; give it as at most 0xFF",
        )
    return f"{sensor:02x}"


def _unsigned(value: int, name: str) -> int:
    if not 0 <= value <= UNSIGNED_MAX:
        raise CommandError(
            f"the {name} {value} is not an unsigned 32-bit number",
        )
    return value


def connect_bus(bus: str) -> DeviceCommand:
    """Build `bus <i2c|spi>`, the first command of a session."""
    if bus not in {"i2c", "spi"}:
        raise CommandError(f"{bus!r} is neither i2c nor spi")
    return DeviceCommand(line=f"bus {bus}", success=f"# bus {bus}")


def connect_payload() -> DeviceCommand:
    """Build `payload 1`, which turns the payload lines on (LP-5)."""
    return DeviceCommand(line="payload 1", success="# payload 1")


def cfg(sensor: int, interval_us: int) -> DeviceCommand:
    """Build `cfg <id_hex> <us>`, which enables or reconfigures a sensor.

    The interval is a positive number of microseconds that fits an unsigned
    32-bit number. The firmware parser accepts zero, but a report interval of
    zero turns the sensor off (1000-3625 §5.4.1), which `cfg` would do while
    the bench keeps the sensor's desired entry, so zero is refused; `off`
    disables a sensor.
    """
    _unsigned(interval_us, "report interval in microseconds")
    if interval_us == 0:
        raise CommandError(
            "the report interval must be above zero, which would turn the sensor "
            "off (1000-3625 §5.4.1); use off to disable it",
        )
    return DeviceCommand(
        line=f"cfg {_hex_sensor(sensor)} {interval_us}",
        success="# ok",
        sensor=sensor,
        interval_us=interval_us,
        enables=True,
    )


def off(sensor: int) -> DeviceCommand:
    """Build `off <id_hex>`, which disables a sensor."""
    return DeviceCommand(
        line=f"off {_hex_sensor(sensor)}",
        success="# ok",
        sensor=sensor,
        enables=False,
    )


def run(duration_ms: int) -> DeviceCommand:
    """Build `run <ms>`, which prints telemetry for that long."""
    _unsigned(duration_ms, "run duration in milliseconds")
    return DeviceCommand(
        line=f"run {duration_ms}",
        success="# run",
        run=True,
    )


def tare(axes: int, basis: int) -> DeviceCommand:
    """Build `tare <axes_hex> <basis>` (1000-3625 §6.4.3.1)."""
    if axes not in _TARE_AXES:
        raise CommandError(f"the axes {axes} are neither 7 (X, Y and Z) nor 4 (Z)")
    if not 0 <= basis <= _TARE_BASIS_MAX:
        raise CommandError(f"the basis {basis} is not between 0 and 5")
    return DeviceCommand(
        line=f"tare {axes:x} {basis}",
        success="# ok",
        needs_ready=True,
    )


def cal(accel: bool, gyro: bool, mag: bool) -> DeviceCommand:
    """Build `cal <accel> <gyro> <mag>` (1000-3625 §6.4.6.1)."""
    flags = "".join("1" if flag else "0" for flag in (accel, gyro, mag))
    return DeviceCommand(
        line=f"cal {flags[0]} {flags[1]} {flags[2]}",
        success="# ok",
        needs_ready=True,
    )


def calget() -> DeviceCommand:
    """Build `calget`, which reads the calibration routine state."""
    return DeviceCommand(line="calget", success="# ok", needs_ready=True)
