# SPDX-License-Identifier: MIT
"""Pure state for the bench stream: samples, replies and stream figures."""

from collections import deque
from collections.abc import Mapping
from dataclasses import dataclass

from pico_gui_bno085.protocol import (
    CalibrationStatus,
    Kind,
    Line,
    RawResponse,
    Reply,
    SaveDcdStatus,
    Telemetry,
)
from pico_gui_bno085.stats import (
    DEFAULT_WINDOW_US,
    Figures,
    Sample,
    StreamWindow,
)

# LP-3: `# ready` is printed each time the session reaches READY. LP-4 does
# not say that the hub's sequence counter continues across runs, so the model
# compares no sequence across a `# ready` or `# run` line.
_RESTART_WORDS = frozenset({"ready", "run"})
# Five seconds at the accelerometer's LP-8 maximum of 500 Hz, the fastest
# sensor the motion plots draw, needs 2501 retained samples, so every
# sensor's history holds that many. The cost is 0.8 to 1.1 MiB per sensor
# by its line's width, so about 11 MiB when thirteen sensors report `V`
# lines and at most about 15 MiB when all thirteen report `G` lines. The
# gyro-integrated rotation vector, at 1000 Hz and not plotted, wraps its
# history in two and a half seconds.
DEFAULT_CAPACITY = 2501

_RESPONSE_RECORDS = (SaveDcdStatus, CalibrationStatus, RawResponse)

__all__ = ("DEFAULT_CAPACITY", "Model", "SensorStats", "Snapshot")


@dataclass(frozen=True)
class SensorStats:
    """What the model knows about one sensor at one instant."""

    sensor: int
    latest: Telemetry
    figures: Figures


@dataclass(frozen=True)
class Snapshot:
    """An immutable copy of the model for a UI refresh."""

    sensors: tuple[SensorStats, ...]
    replies: int
    rejected: int
    last_reply: Reply | None
    calibration: CalibrationStatus | None
    response_line: str


class _Track:
    """The mutable record behind one sensor's `SensorStats`."""

    def __init__(self, first: Telemetry, capacity: int, window_us: int) -> None:
        self.latest = first
        self.history: deque[Telemetry] = deque(maxlen=capacity)
        self.window = StreamWindow(window_us)


class Model:
    """Latest sample per sensor, history, and windowed stream figures.

    The figures come from the event times the lines carry, so the model
    needs no clock of its own; the window prunes by event time.
    """

    def __init__(
        self,
        *,
        capacity: int = DEFAULT_CAPACITY,
        window_us: int = DEFAULT_WINDOW_US,
    ) -> None:
        if capacity < 1 or window_us <= 0:
            raise ValueError("capacity and window must be positive")
        self._capacity = capacity
        self._window_us = window_us
        self._tracks: dict[int, _Track] = {}
        self._restart_pending: set[int] = set()
        self._replies = 0
        self._rejected = 0
        self._last_reply: Reply | None = None
        self._calibration: CalibrationStatus | None = None
        self._response_line = ""

    def feed(self, line: Line, text: str) -> None:
        """Take one parsed line; `text` is the line as it was received."""
        if isinstance(line, Telemetry):
            self._telemetry(line)
            return
        self._replies += 1
        if isinstance(line, Reply):
            self._last_reply = line
            if line.word in _RESTART_WORDS:
                self._restart_pending = set(self._tracks)
        if isinstance(line, CalibrationStatus):
            self._calibration = line
        if isinstance(line, _RESPONSE_RECORDS):
            self._response_line = text

    def reject(self) -> None:
        """Count a line that the parser refused."""
        self._rejected += 1

    def history(self, sensor: int) -> tuple[Telemetry, ...]:
        """Return the retained samples of `sensor`, oldest first."""
        track = self._tracks.get(sensor)
        return () if track is None else tuple(track.history)

    def snapshot(
        self,
        commanded: Mapping[int, int] | None = None,
    ) -> Snapshot:
        """Return the state, with sensors in ascending order.

        `commanded` maps a sensor to the report interval this session asked
        for with `cfg`; a sensor absent from it takes the median observed
        interval as its nominal.
        """
        wanted: Mapping[int, int] = {} if commanded is None else commanded
        sensors = tuple(
            SensorStats(
                sensor,
                track.latest,
                track.window.figures(sensor, wanted.get(sensor)),
            )
            for sensor, track in sorted(self._tracks.items())
        )
        return Snapshot(
            sensors,
            self._replies,
            self._rejected,
            self._last_reply,
            self._calibration,
            self._response_line,
        )

    def _telemetry(self, sample: Telemetry) -> None:
        track = self._tracks.get(sample.sensor)
        if track is None:
            track = _Track(sample, self._capacity, self._window_us)
            self._tracks[sample.sensor] = track
        restart = sample.sensor in self._restart_pending
        if restart:
            self._restart_pending.discard(sample.sensor)
        track.latest = sample
        track.history.append(sample)
        # The G line's report has no sequence or status field
        # (1000-3625 §6.5.44.2), so its sequence and accuracy are
        # placeholders and stay out of the gap figures.
        track.window.add(
            Sample(
                sample.time_us,
                sample.seq,
                sample.kind is not Kind.G,
                restart,
            ),
        )
