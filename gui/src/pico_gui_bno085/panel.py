# SPDX-License-Identifier: MIT
"""The BNO085 page: the stream, its health, and the bench controls."""

import math
import time
from collections.abc import Callable, Container, Mapping
from dataclasses import dataclass

from nicegui import ui

from pico_gui.commands import CommandError
from pico_gui.session import Status
from pico_gui.shell import (
    REFRESH_INTERVAL_SECONDS,
    ConnectionBar,
    StatusLine,
)
from pico_gui.theme import apply_theme
from pico_gui.transport import TransportError
from pico_gui_bno085.charts import (
    MOTION_PLOTS,
    WINDOW_SECONDS,
    Motion,
    motion_plot,
)
from pico_gui_bno085.commands import (
    MAX_RATE_HZ,
    TARE_AXES_NAMES,
    TARE_BASIS_NAMES,
    UNCONFIRMED_RATES,
    DeviceCommand,
    cal,
    calget,
    cfg,
    off,
    tare,
)
from pico_gui_bno085.controller import ControllerState, Phase
from pico_gui_bno085.model import SensorStats, Snapshot
from pico_gui_bno085.orientation import Orientation, orientation_of
from pico_gui_bno085.protocol import CalibrationStatus, Kind, Telemetry

# LP-6 names each sample kind and lists the sensor IDs that print it; these
# names are the SH-2 report names for those IDs.
_SENSOR_NAMES = {
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

# LP-4: the accuracy names of the status bits. The G line's report carries a
# placeholder accuracy (1000-3625 §6.5.44.2), so it reports none.
ACCURACY_NAMES = ("unreliable", "low", "medium", "high")
_G_ACCURACY = "not reported"

# The reason every command control gives when the source is a recording: a
# replay has no board to command.
NO_BOARD = "commands need a board; this session replays a recording"

_DEFAULT_INTERVAL_US = 10_000

# A chart redraws at most this often: its option set is the largest message
# the page sends, and ten drawings a second read as continuous motion.
CHART_INTERVAL_SECONDS = 0.1

# The three rotation sources the page offers, all of LP-6's rotation kinds
# the bench prints: the rotation vector (`Q`), the game rotation vector
# (`R`) and the gyro-integrated rotation vector (`G`).
_ROTATION_SOURCES = {
    0x05: "Rotation vector",
    0x08: "Game rotation vector",
    0x2A: "Gyro-integrated rotation vector",
}
_DEFAULT_ROTATION_SOURCE = 0x05

# One sentence on the reading every rotation view takes (LP-6).
_ROTATION_UNCONFIRMED = (
    "LP-6 does not confirm the direction of the rotation the quaternion "
    "expresses or the axes of its reference frame, so this view may show "
    "the inverse orientation."
)
# The primes of the moved axes are escaped so the confusable-character
# check does not see them; the line renders with a prime on the Y and a
# double prime on the X.
_EULER_CONVENTION = (
    "Intrinsic Z-Y\u2032-X\u2033 angles: yaw about Z, then pitch about the new Y, "
    "then roll about the newest X, in degrees; yaw and roll in (-180, 180], "
    "pitch in [-90, 90]."
)
_NO_ORIENTATION = "the latest sample carries no orientation"

# The device axes of the board view (LP-6 frames: X to the right, Y along
# the face, Z out of the front face), drawn from the board's centre along
# each face. The colours follow the scene's own axes helper: X red, Y
# green, Z blue.
_BOARD_AXES = (
    ((1.3, 0.0, 0.0), "X", "#e74c3c"),
    ((0.0, 1.8, 0.0), "Y", "#2ecc71"),
    ((0.0, 0.0, 1.0), "Z", "#3498db"),
)
_BOARD_COLOUR = "#5b6770"

__all__ = (
    "ACCURACY_NAMES",
    "CHART_INTERVAL_SECONDS",
    "HEALTH_COLUMNS",
    "NO_BOARD",
    "CommandSurface",
    "Page",
    "RecordingSurface",
    "ReplaySurface",
    "Sources",
    "accuracy_text",
    "build_page",
    "calibration_text",
    "commanded_hz",
    "controller_text",
    "convention_text",
    "euler_label",
    "health_rows",
    "interval_us",
    "missing_rotation_text",
    "motion_options",
    "motion_window_text",
    "no_motion_text",
    "progress_text",
    "recording_text",
    "reference_text",
    "response_text",
    "rotation_options",
    "sensor_name",
    "sensor_options",
    "sensor_rows",
    "summary",
)


@dataclass(frozen=True)
class CommandSurface:
    """How the page asks the session to run one bench command.

    `retry` asks for the connect sequence again after a refusal, a timeout
    or a send failure.
    """

    submit: Callable[[DeviceCommand], None]
    state: Callable[[], ControllerState]
    retry: Callable[[], None]


@dataclass(frozen=True)
class ReplaySurface:
    """How the page holds and restarts a replay."""

    pause: Callable[[], None]
    resume: Callable[[], None]
    restart: Callable[[], None]
    progress: Callable[[], float]
    paused: Callable[[], bool]


@dataclass(frozen=True)
class RecordingSurface:
    """How the page starts and stops a recording of the received lines."""

    start: Callable[[], None]
    stop: Callable[[], None]
    active: Callable[[], bool]
    name: Callable[[], str]
    failure: Callable[[], str]


@dataclass(frozen=True)
class Sources:
    """Everything the page reads or drives; the launcher or a test builds it.

    `history` returns one sensor's retained samples, oldest first, and
    feeds the motion plots. `commands` is none when the source is a
    recording, and every command control is then disabled with the reason
    above. `replay` and `recording` are none when the launcher was given
    no recording to drive and no directory to write into.
    """

    label: str
    status: Callable[[], Status]
    snapshot: Callable[[], Snapshot]
    history: Callable[[int], tuple[Telemetry, ...]]
    tick: Callable[[], None]
    commands: CommandSurface | None
    replay: ReplaySurface | None
    recording: RecordingSurface | None


def sensor_name(sensor: int) -> str:
    """Return the display name of a sensor ID."""
    name = _SENSOR_NAMES.get(sensor, "Sensor")
    return f"{name} (0x{sensor:02X})"


def sensor_options() -> dict[int, str]:
    """Return the select options of the sensors the bench can enable."""
    return {
        sensor: f"{sensor_name(sensor)}, at most {rate} Hz"
        + (", rate unconfirmed" if sensor in UNCONFIRMED_RATES else "")
        for sensor, rate in sorted(MAX_RATE_HZ.items())
    }


def rotation_options() -> dict[int, str]:
    """Return the select options of the three rotation sources."""
    return {
        sensor: f"{name} (0x{sensor:02X})"
        for sensor, name in sorted(_ROTATION_SOURCES.items())
    }


def reference_text(sensor: int) -> str:
    """Return the line under the view: what the source is referenced to."""
    if sensor == 0x05:
        return (
            "The rotation vector (0x05) is referenced to magnetic north "
            "and gravity (LP-6)."
        )
    if sensor == 0x08:
        return (
            "The game rotation vector (0x08) is referenced to gravity alone "
            "and has no heading reference (LP-6)."
        )
    return (
        "The gyro-integrated rotation vector (0x2A) is UNCONFIRMED in the "
        "frame it is referenced to (LP-6)."
    )


def convention_text(sensor: int) -> str:
    """Return the Euler convention line, with the game source's caveat."""
    if sensor == 0x08:
        return f"{_EULER_CONVENTION} Yaw has no heading reference."
    return _EULER_CONVENTION


def _degrees_text(value: float) -> str:
    """Return `value` as a degree reading, without a sign on a zero.

    A reading that rounds to zero is a zero whatever side of it the
    arithmetic landed on, so the readout never shows `-0.00`.
    """
    text = f"{value:.2f}"
    return text[1:] if text == "-0.00" else text


def euler_label(
    sensor: int,
    orientation: Orientation | None,
    reported: bool,
) -> str:
    """Return the Euler readout line of the selected source.

    `reported` is false when the source has printed no sample at all, and
    an orientation of none is a sample whose quaternion is no rotation.
    """
    if not reported:
        return f"no samples of {sensor_name(sensor)} yet"
    if orientation is None:
        return _NO_ORIENTATION
    euler = orientation.euler
    return (
        f"yaw {_degrees_text(euler.yaw_deg)}°, "
        f"pitch {_degrees_text(euler.pitch_deg)}°, "
        f"roll {_degrees_text(euler.roll_deg)}°"
    )


def missing_rotation_text(sensor: int) -> str:
    """Return the note when the selected source has printed nothing."""
    return f"no samples yet; enable {sensor_name(sensor)} to see the board"


def no_motion_text(sensor: int) -> str:
    """Return the note of a plot whose sensor has printed no `V` line."""
    return f"no samples yet for {sensor_name(sensor)}"


def motion_window_text(sensor: int) -> str:
    """Return the note of a plot that is drawing its window."""
    unit = MOTION_PLOTS[sensor].unit
    return f"{unit} against event time in seconds, the latest {WINDOW_SECONDS:g} s"


def motion_options(sensor: int, motion: Motion | None) -> dict[str, object]:
    """Return the ECharts options of one sensor's motion plot."""
    newest = 0.0 if motion is None else motion.newest_s
    series: list[dict[str, object]] = []
    for index, axis in enumerate("xyz"):
        points: list[list[float]] = []
        if motion is not None:
            points = [[time_s, value] for time_s, value in motion.points[index]]
        series.append(
            {
                "name": axis,
                "type": "line",
                "showSymbol": False,
                "data": points,
            },
        )
    return {
        "animation": False,
        "legend": {"top": 0},
        "grid": {"left": 56, "right": 12, "top": 28, "bottom": 28},
        "tooltip": {"trigger": "axis"},
        "xAxis": {
            "type": "value",
            "name": "event time (s)",
            "min": newest - WINDOW_SECONDS,
            "max": newest,
        },
        "yAxis": {"type": "value", "name": MOTION_PLOTS[sensor].unit},
        "series": series,
    }


def interval_us(value: float | None) -> int:
    """Return the report interval the interval input stands for, or raise."""
    if value is None:
        raise CommandError("the report interval is empty")
    if not math.isfinite(value) or value <= 0 or value != int(value):
        raise CommandError("the report interval is a whole number of microseconds")
    return int(value)


def accuracy_text(stats: SensorStats) -> str:
    """Return the latest accuracy of one sensor as its LP-4 name."""
    if stats.latest.kind is Kind.G:
        return _G_ACCURACY
    return ACCURACY_NAMES[stats.latest.accuracy]


def commanded_hz(commanded_us: int) -> str:
    """Return the rate a commanded interval asks for, to the nearest hertz."""
    return f"{round(1_000_000 / commanded_us)} Hz"


def sensor_rows(
    snapshot: Snapshot,
    enabled: Container[int],
    commanded: Mapping[int, int],
) -> list[dict[str, str | int]]:
    """Return the rows of the sensors table."""
    rows: list[dict[str, str | int]] = []
    for stats in snapshot.sensors:
        interval = commanded.get(stats.sensor)
        rows.append(
            {
                "sensor": sensor_name(stats.sensor),
                "state": "enabled" if stats.sensor in enabled else "off",
                "commanded": "—" if interval is None else commanded_hz(interval),
                "rate": f"{stats.figures.rate_hz:.1f}",
                "count": stats.figures.count,
                "accuracy": accuracy_text(stats),
            },
        )
    return rows


def health_rows(snapshot: Snapshot) -> list[dict[str, str | int]]:
    """Return the rows of the stream health table."""
    rows: list[dict[str, str | int]] = []
    for stats in snapshot.sensors:
        figures = stats.figures
        basis = "commanded" if figures.nominal_is_commanded else "observed"
        rows.append(
            {
                "sensor": sensor_name(stats.sensor),
                "rate": f"{figures.rate_hz:.1f}",
                "missing": figures.missing,
                "repeats": figures.repeats,
                "inversions": figures.timestamps.inversions,
                "backward": figures.timestamps.worst_backward_us,
                "clock_faults": figures.clock_faults,
                "jitter": f"{figures.timestamps.p99_ratio:.3f}",
                "nominal": f"{figures.nominal_interval_us} µs ({basis})",
            },
        )
    return rows


def controller_text(state: ControllerState) -> str:
    """Return the one-line state of the command path."""
    text = f"commands: {state.phase.value}"
    if state.in_flight is not None:
        text += f", awaiting the reply to '{state.in_flight}'"
    if state.queued:
        text += f", {state.queued} queued"
    if state.detail:
        text += f"; {state.detail}"
    return text


def calibration_text(calibration: CalibrationStatus | None) -> str:
    """Return the decoded state of the last `# cal` line."""
    if calibration is None:
        return "no calibration response yet"
    routines = "; ".join(
        f"{name} {'on' if flag else 'off'}"
        for name, flag in (
            ("accelerometer", calibration.accel),
            ("gyroscope", calibration.gyro),
            ("magnetometer", calibration.mag),
        )
    )
    return (
        f"status {calibration.status} (0 is success); {routines}; "
        f"planar accelerometer {'on' if calibration.planar else 'off'}; "
        f"on-table {'on' if calibration.table else 'off'}"
    )


def response_text(line: str) -> str:
    """Return the last command response as it was received."""
    return f"last command response: {line}" if line else "no command response yet"


def progress_text(progress: float, paused: bool) -> str:
    """Return the replay progress as one line."""
    text = f"replay at {progress:.0%}"
    if paused:
        return text + ", held"
    return text


def recording_text(active: bool, name: str, message: str) -> str:
    """Return the recording state as one line.

    `message` is the control's last word when no recording is running: a
    refusal to start one that is already active, or the reason one failed.
    """
    if active:
        return f"recording to {name}"
    if message:
        return message
    return "not recording"


def summary(snapshot: Snapshot) -> str:
    """Return the line shown below the tables."""
    return (
        f"{snapshot.replies} replies, "
        f"{snapshot.rejected} lines not part of the protocol"
    )


_SENSOR_COLUMNS = [
    {"name": "sensor", "label": "Sensor", "field": "sensor", "align": "left"},
    {"name": "state", "label": "State", "field": "state", "align": "left"},
    {"name": "commanded", "label": "Commanded", "field": "commanded", "align": "right"},
    {"name": "rate", "label": "Rate (Hz)", "field": "rate", "align": "right"},
    {"name": "count", "label": "Samples", "field": "count", "align": "right"},
    {"name": "accuracy", "label": "Accuracy", "field": "accuracy", "align": "left"},
]

HEALTH_COLUMNS = [
    {"name": "sensor", "label": "Sensor", "field": "sensor", "align": "left"},
    {"name": "rate", "label": "Rate (Hz)", "field": "rate", "align": "right"},
    {"name": "missing", "label": "Missing", "field": "missing", "align": "right"},
    {"name": "repeats", "label": "Repeats", "field": "repeats", "align": "right"},
    {
        "name": "inversions",
        "label": "Inversions",
        "field": "inversions",
        "align": "right",
    },
    {
        "name": "backward",
        "label": "Worst back (µs)",
        "field": "backward",
        "align": "right",
    },
    {
        "name": "clock_faults",
        "label": "Clock faults (back ≥ 400 µs)",
        "field": "clock_faults",
        "align": "right",
    },
    {
        "name": "jitter",
        "label": "p99 jitter (intervals)",
        "field": "jitter",
        "align": "right",
    },
    {
        "name": "nominal",
        "label": "Nominal interval",
        "field": "nominal",
        "align": "right",
    },
]


class Page:
    """The built page; `refresh` reads the sources and updates every widget."""

    def __init__(
        self,
        sources: Sources,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self._sources = sources
        self._clock = clock
        self._drawn: dict[int, tuple[int, int]] = {}
        self._drawn_at: dict[int, float] = {}
        self._sensor_rows: list[dict[str, str | int]] | None = None
        self._health_rows: list[dict[str, str | int]] | None = None
        apply_theme()
        self.bar = ConnectionBar(sources.label)
        with ui.row().classes("w-full items-start"):
            with ui.column().classes("col-12 col-md"):
                with ui.row().classes("items-center"):
                    self.controller = ui.label()
                    if sources.commands is not None:
                        self.retry = ui.button(
                            "Retry connect",
                            on_click=self._retry_clicked,
                        ).mark("retry")
                        self.retry.disable()
                self.line = StatusLine()
                self.note = StatusLine()
                with ui.row().classes("w-full items-stretch"):
                    self._build_orientation()
                    self._build_euler()
                self._build_motion()
            self._build_tabs()
        if sources.commands is None:
            self.controller.set_text(NO_BOARD)
        ui.timer(REFRESH_INTERVAL_SECONDS, self.refresh)

    def _build_tabs(self) -> None:
        """Build the tab panel of controls, one group visible at a time.

        Sensors is the default because enabling a sensor is the first thing
        a bench session needs: nothing else shows data until one reports.
        The stream health table and the calibration controls follow; Recording
        and Replay appear only with their sources.
        """
        groups: list[tuple[str, str, Callable[[], None]]] = [
            ("Sensors", "sensors", self._build_sensors),
            ("Health", "health", self._build_health),
            ("Calibration", "calibration", self._build_calibration),
        ]
        if self._sources.recording is not None:
            groups.append(("Recording", "recording", self._build_recording))
        if self._sources.replay is not None:
            groups.append(("Replay", "replay", self._build_replay))
        with ui.column().classes("col-12 col-md-4"):
            with ui.tabs().classes("w-full") as tabs:
                for label, name, _ in groups:
                    ui.tab(name, label).mark(f"tab-{name}")
            with ui.tab_panels(tabs, value="sensors").classes("w-full"):
                for _, name, build in groups:
                    with ui.tab_panel(name):
                        build()

    def _build_sensors(self) -> None:
        """Build the sensors table and the controls that enable a sensor."""
        self.sensors = ui.table(
            columns=_SENSOR_COLUMNS,
            rows=[],
            row_key="sensor",
        ).classes("w-full")
        with ui.row():
            self.sensor_choice = ui.select(
                options=sensor_options(),
                value=0x01,
                label="sensor",
            )
            self.interval = ui.number(
                label="report interval (µs)",
                value=_DEFAULT_INTERVAL_US,
            )
            self.enable = self._board_control(
                "Enable",
                self._enable_clicked,
            ).mark("enable")
            self.disable = self._board_control(
                "Disable",
                self._disable_clicked,
            ).mark("disable")

    def _build_health(self) -> None:
        """Build the stream health table."""
        ui.label("Stream health").classes("text-lg")
        self.health = ui.table(
            columns=HEALTH_COLUMNS,
            rows=[],
            row_key="sensor",
        ).classes("w-full")

    def _build_orientation(self) -> None:
        """Build the board view, its source select and the lines beside it."""
        with ui.card().classes("col"):
            ui.label("Board orientation").classes("text-lg")
            with ui.row().classes("items-center"):
                self.rotation_choice = ui.select(
                    options=rotation_options(),
                    value=_DEFAULT_ROTATION_SOURCE,
                    label="rotation source",
                )
                self.orientation_state = ui.label().classes("text-sm text-grey")
            self._build_scene()
            self.reference = ui.label().classes("text-sm text-grey")
            self.unconfirmed = ui.label(_ROTATION_UNCONFIRMED).classes(
                "text-sm text-grey",
            )

    def _build_scene(self) -> None:
        """Build the scene and the board group one rotation turns whole.

        The scene's own axes: NiceGUI's scene points the camera up along
        +Z and places the camera at (0, -3, 5)
        (`nicegui/elements/scene/scene.js:86-87`; the same defaults sit in
        `nicegui/elements/scene/scene.py:30-37`), so +Z is up, the viewer
        looks along +Y and the grid lies in the XY plane (`scene.js:155-158`).
        The device frame of LP-6 — X to the right, Y along the face, Z
        out of the front face — maps one-to-one onto the scene's X, Y and
        Z, so the board drawn flat shows its front face up. `rotate_R`'s
        matrix becomes the object's own rotation (`scene.js:29-35`).
        """
        with ui.scene(width=340, height=280) as scene, scene.group() as self.board:
            scene.box(2.0, 3.0, 0.3).material(_BOARD_COLOUR)
            for tip, name, colour in _BOARD_AXES:
                scene.line([0.0, 0.0, 0.0], list(tip)).material(colour)
                scene.text(
                    name,
                    f"font-size: 14px; font-weight: bold; color: {colour}",
                ).move(*tip)

    def _build_euler(self) -> None:
        """Build the Euler readouts of the selected rotation source."""
        with ui.card().classes("col"):
            ui.label("Euler angles").classes("text-lg")
            self.euler = ui.label()
            self.convention = ui.label().classes("text-sm text-grey")

    def _build_motion(self) -> None:
        """Build one plot card per plotted vector sensor, in one row."""
        self.motion: dict[int, ui.echart] = {}
        self.motion_notes: dict[int, ui.label] = {}
        with ui.row().classes("w-full items-stretch"):
            for sensor, spec in MOTION_PLOTS.items():
                with ui.card().classes("col").style("min-width: 14rem"):
                    ui.label(f"{spec.label} (0x{sensor:02X})").classes("text-lg")
                    self.motion_notes[sensor] = ui.label().classes(
                        "text-sm text-grey",
                    )
                    self.motion[sensor] = ui.echart(
                        options=motion_options(sensor, None),
                    )

    def _build_calibration(self) -> None:
        """Build the calibration state and the tare and calibrate controls."""
        ui.label("Accuracy and calibration").classes("text-lg")
        self.calibration = ui.label()
        self.response = ui.label().classes("text-sm text-grey")
        with ui.row():
            self.axes = ui.select(
                options=dict(TARE_AXES_NAMES),
                value=0x7,
                label="tare axes",
            )
            self.basis = ui.select(
                options=dict(TARE_BASIS_NAMES),
                value=0,
                label="tare basis",
            )
            self.tare = self._board_control(
                "Tare",
                self._tare_clicked,
            ).mark("tare")
        with ui.row():
            self.accel = ui.switch("accelerometer", value=True).mark("accel")
            self.gyro = ui.switch("gyroscope", value=True).mark("gyro")
            self.mag = ui.switch("magnetometer", value=True).mark("mag")
            self.calibrate = self._board_control(
                "Calibrate",
                self._calibrate_clicked,
            ).mark("calibrate")
            self.calget = self._board_control(
                "Read calibration state",
                self._calget_clicked,
            ).mark("calget")
        ui.label(
            "Calibrate also turns the planar-accelerometer and on-table routines off.",
        ).classes("text-sm text-grey")

    def _build_recording(self) -> None:
        """Build the record button and its state line."""
        ui.label("Recording").classes("text-lg")
        self.recording_state = ui.label()
        with ui.row():
            self.record_start = self._any_control(
                "Start",
                self._record_start_clicked,
            ).mark("record-start")
            self.record_stop = self._any_control(
                "Stop",
                self._record_stop_clicked,
            ).mark("record-stop")

    def _build_replay(self) -> None:
        """Build the replay progress and its hold and restart controls."""
        ui.label("Replay").classes("text-lg")
        self.progress = ui.label()
        with ui.row():
            self.hold = self._any_control("Hold", self._hold_clicked).mark(
                "hold",
            )
            self.restart = self._any_control(
                "Restart",
                self._restart_clicked,
            ).mark("restart")

    def refresh(self) -> None:
        """Read the sources and update every widget; nothing here raises."""
        sources = self._sources
        sources.tick()
        current = sources.snapshot()
        self.bar.update(sources.status())
        commands = sources.commands
        if commands is None:
            self._show_sensors(sensor_rows(current, (), {}))
        else:
            state = commands.state()
            self.controller.set_text(controller_text(state))
            if state.phase is Phase.REFUSED:
                self.retry.enable()
            else:
                self.retry.disable()
            self._show_sensors(
                sensor_rows(current, state.enabled, state.commanded),
            )
        self._refresh_orientation(current)
        rows = health_rows(current)
        if rows != self._health_rows:
            self._health_rows = rows
            self.health.rows = rows
        self._refresh_motion(current)
        self.calibration.set_text(calibration_text(current.calibration))
        self.response.set_text(response_text(current.response_line))
        if sources.recording is not None:
            self._refresh_recording(sources.recording)
        if sources.replay is not None:
            self._refresh_replay(sources.replay)
        self.line.update(summary(current))

    def _show_sensors(self, rows: list[dict[str, str | int]]) -> None:
        """Send the sensors table to the browser only when its rows changed.

        Assigning `rows` sends the table; assigning an equal list would
        send it again.
        """
        if rows != self._sensor_rows:
            self._sensor_rows = rows
            self.sensors.rows = rows

    def _refresh_orientation(self, current: Snapshot) -> None:
        """Turn the board to the selected source's latest sample."""
        sensor = int(self.rotation_choice.value)
        stats = next(
            (entry for entry in current.sensors if entry.sensor == sensor),
            None,
        )
        self.reference.set_text(reference_text(sensor))
        self.convention.set_text(convention_text(sensor))
        if stats is None:
            self.orientation_state.set_text(missing_rotation_text(sensor))
            self.euler.set_text(euler_label(sensor, None, False))
            return
        orientation = orientation_of(stats.latest.values)
        if orientation is None:
            self.orientation_state.set_text(_NO_ORIENTATION)
        else:
            self.orientation_state.set_text("")
            self.board.rotate_R([list(row) for row in orientation.matrix])
        self.euler.set_text(euler_label(sensor, orientation, True))

    def _refresh_motion(self, current: Snapshot) -> None:
        """Redraw each plot whose sensor has a new sample, at most 10 Hz.

        A sensor whose latest sample has not changed is left alone, and a
        chart drawn less than `CHART_INTERVAL_SECONDS` ago keeps its drawing
        until a later refresh. A sensor with no payload line left to draw
        shows an empty chart and the "no samples" note. After a replay
        restart the model is empty, so every note reads "no samples", but
        each chart keeps its last drawing until a sample arrives that
        differs from the one last drawn.
        """
        latest = {stats.sensor: stats.latest for stats in current.sensors}
        for sensor, chart in self.motion.items():
            sample = latest.get(sensor)
            if sample is None:
                self.motion_notes[sensor].set_text(no_motion_text(sensor))
                continue
            if self._drawn.get(sensor) == (sample.time_us, sample.seq):
                continue
            now = self._clock()
            if now - self._drawn_at.get(sensor, -math.inf) < CHART_INTERVAL_SECONDS:
                continue
            motion = motion_plot(self._sources.history(sensor))
            self._drawn[sensor] = (sample.time_us, sample.seq)
            self._drawn_at[sensor] = now
            chart.options.clear()
            chart.options.update(motion_options(sensor, motion))
            chart.update()
            self.motion_notes[sensor].set_text(
                no_motion_text(sensor)
                if motion is None
                else motion_window_text(sensor),
            )

    def _refresh_recording(self, recording: RecordingSurface) -> None:
        active = recording.active()
        self.recording_state.set_text(
            recording_text(active, recording.name(), recording.failure()),
        )
        if active:
            self.record_start.disable()
        else:
            self.record_start.enable()

    def _refresh_replay(self, replay: ReplaySurface) -> None:
        paused = replay.paused()
        self.progress.set_text(progress_text(replay.progress(), paused))
        self.hold.set_text("Resume" if paused else "Hold")

    def _board_control(self, text: str, handler: Callable[[], None]) -> ui.button:
        """Build a button that needs a board, disabled when there is none."""
        button = ui.button(text, on_click=handler)
        if self._sources.commands is None:
            button.disable()
        return button

    def _any_control(self, text: str, handler: Callable[[], None]) -> ui.button:
        """Build a button that works whatever the source is."""
        return ui.button(text, on_click=handler)

    def _say(self, message: str) -> None:
        self.note.update(message)

    def _submit(self, command: DeviceCommand) -> None:
        commands = self._sources.commands
        if commands is None:
            return
        try:
            commands.submit(command)
        except CommandError as error:
            self._say(str(error))

    def _enable_clicked(self) -> None:
        sensor = int(self.sensor_choice.value)
        try:
            self._submit(cfg(sensor, interval_us(self.interval.value)))
        except CommandError as error:
            self._say(str(error))

    def _disable_clicked(self) -> None:
        self._submit(off(int(self.sensor_choice.value)))

    def _tare_clicked(self) -> None:
        axes, basis = self.axes.value, self.basis.value
        if axes is None or basis is None:
            self._say("choose the tare axes and basis first")
            return
        try:
            command = tare(int(axes), int(basis))
        except CommandError as error:
            self._say(str(error))
            return
        self._submit(command)

    def _calibrate_clicked(self) -> None:
        # `cal` takes three booleans and cannot be refused on the host, so
        # the build needs no guard of its own.
        self._submit(
            cal(
                bool(self.accel.value),
                bool(self.gyro.value),
                bool(self.mag.value),
            ),
        )

    def _calget_clicked(self) -> None:
        self._submit(calget())

    def _record_start_clicked(self) -> None:
        recording = self._sources.recording
        if recording is None:
            return
        try:
            recording.start()
        except TransportError as error:
            self._say(str(error))

    def _record_stop_clicked(self) -> None:
        recording = self._sources.recording
        if recording is None:
            return
        try:
            recording.stop()
        except TransportError as error:
            self._say(str(error))

    def _retry_clicked(self) -> None:
        commands = self._sources.commands
        if commands is None:
            return
        commands.retry()

    def _hold_clicked(self) -> None:
        replay = self._sources.replay
        if replay is None:
            return
        if replay.paused():
            replay.resume()
        else:
            replay.pause()

    def _restart_clicked(self) -> None:
        replay = self._sources.replay
        if replay is None:
            return
        try:
            replay.restart()
        except TransportError as error:
            self._say(str(error))


def build_page(sources: Sources) -> Page:
    """Build the page for one client.

    A timer refreshes it at most 30 times a second; the tables, the readouts
    and the board view send only what changed, and a chart is drawn at most
    10 times a second.
    """
    return Page(sources)
