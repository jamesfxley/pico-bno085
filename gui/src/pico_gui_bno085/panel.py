# SPDX-License-Identifier: MIT
"""The minimal panel: connection state and live per-sensor rates."""

from collections.abc import Callable

from nicegui import ui

from pico_gui.session import Status
from pico_gui.shell import (
    REFRESH_INTERVAL_SECONDS,
    ConnectionBar,
    StatusLine,
)
from pico_gui.theme import apply_theme
from pico_gui_bno085.model import SensorStats, Snapshot

# LP-6 names each sample kind and lists the sensor IDs that print it; these
# names are the SH-2 report names for those IDs.
SENSOR_NAMES = {
    0x01: "Accelerometer",
    0x02: "Gyroscope",
    0x03: "Magnetometer",
    0x04: "Linear acceleration",
    0x05: "Rotation vector",
    0x06: "Gravity",
    0x07: "Uncalibrated gyroscope",
    0x08: "Game rotation vector",
    0x09: "Geomagnetic rotation vector",
    0x0F: "Uncalibrated magnetometer",
    0x28: "ARVR-stabilised rotation vector",
    0x29: "ARVR-stabilised game rotation vector",
    0x2A: "Gyro-integrated rotation vector",
}

COLUMNS = [
    {"name": "sensor", "label": "Sensor", "field": "sensor", "align": "left"},
    {"name": "rate", "label": "Rate (Hz)", "field": "rate", "align": "right"},
    {"name": "count", "label": "Samples", "field": "count", "align": "right"},
    {"name": "missing", "label": "Missing", "field": "missing", "align": "right"},
]


def sensor_name(sensor: int) -> str:
    """Return the display name of a sensor ID."""
    name = SENSOR_NAMES.get(sensor, "Sensor")
    return f"{name} (0x{sensor:02X})"


def row_of(stats: SensorStats) -> dict[str, str | int]:
    """Return one table row for a sensor."""
    return {
        "sensor": sensor_name(stats.sensor),
        "rate": f"{stats.rate_hz:.1f}",
        "count": stats.count,
        "missing": stats.missing,
    }


def summary(snapshot: Snapshot) -> str:
    """Return the line shown under the table."""
    return (
        f"{snapshot.replies} replies, "
        f"{snapshot.rejected} lines not part of the protocol"
    )


def build(
    source: str,
    status: Callable[[], Status],
    snapshot: Callable[[], Snapshot],
) -> None:
    """Build the page. A timer refreshes it at most 30 times a second."""
    apply_theme()
    bar = ConnectionBar(source)
    table = ui.table(columns=COLUMNS, rows=[], row_key="sensor").classes("w-full")
    line = StatusLine()

    def refresh() -> None:
        current = snapshot()
        bar.update(status())
        table.rows = [row_of(stats) for stats in current.sensors]
        table.update()
        line.update(summary(current))

    ui.timer(REFRESH_INTERVAL_SECONDS, refresh)
