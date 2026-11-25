# SPDX-License-Identifier: MIT
"""Pure state for the bench stream: latest sample, history, rates and gaps."""

from collections import deque
from dataclasses import dataclass

from pico_gui_bno085.protocol import (
    SEQUENCE_MODULUS,
    Kind,
    Line,
    Reply,
    Telemetry,
)

# LP-3: `# ready` is printed each time the session reaches READY. LP-4: events
# print only between `run` and its `# done`, so a jump in a sensor's sequence
# across two runs is not a loss.
RESTART_WORDS = frozenset({"ready", "run"})
DEFAULT_CAPACITY = 2048
DEFAULT_WINDOW_SECONDS = 1.0


@dataclass(frozen=True)
class SensorStats:
    """What the model knows about one sensor at one instant."""

    sensor: int
    latest: Telemetry
    count: int
    rate_hz: float
    missing: int
    repeats: int


@dataclass(frozen=True)
class Snapshot:
    """An immutable copy of the model for a UI refresh."""

    sensors: tuple[SensorStats, ...]
    replies: int
    rejected: int
    last_reply: Reply | None


class _Track:
    """The mutable record behind one sensor's `SensorStats`."""

    def __init__(self, first: Telemetry, now: float, capacity: int) -> None:
        self.latest = first
        self.history: deque[Telemetry] = deque(maxlen=capacity)
        self.arrivals: deque[float] = deque()
        self.started = now
        self.count = 0
        self.missing = 0
        self.repeats = 0
        self.previous_seq: int | None = None


class Model:
    """Latest sample per sensor, ring buffers and rate and gap statistics.

    Time comes from the caller, so a test needs no clock.
    """

    def __init__(
        self,
        *,
        capacity: int = DEFAULT_CAPACITY,
        window_seconds: float = DEFAULT_WINDOW_SECONDS,
    ) -> None:
        if capacity < 1 or window_seconds <= 0:
            raise ValueError("capacity and window must be positive")
        self._capacity = capacity
        self._window = window_seconds
        self._tracks: dict[int, _Track] = {}
        self._replies = 0
        self._rejected = 0
        self._last_reply: Reply | None = None

    def feed(self, line: Line, now: float) -> None:
        """Take one parsed line that arrived at `now` seconds."""
        if isinstance(line, Telemetry):
            self._telemetry(line, now)
            return
        self._replies += 1
        if isinstance(line, Reply):
            self._last_reply = line
            if line.word in RESTART_WORDS:
                self._restart_sequences()

    def reject(self) -> None:
        """Count a line that the parser refused."""
        self._rejected += 1

    def history(self, sensor: int) -> tuple[Telemetry, ...]:
        """Return the retained samples of `sensor`, oldest first."""
        track = self._tracks.get(sensor)
        return () if track is None else tuple(track.history)

    def snapshot(self, now: float) -> Snapshot:
        """Return the state at `now`, with sensors in ascending order."""
        sensors = tuple(
            SensorStats(
                sensor,
                track.latest,
                track.count,
                self._rate(track, now),
                track.missing,
                track.repeats,
            )
            for sensor, track in sorted(self._tracks.items())
        )
        return Snapshot(sensors, self._replies, self._rejected, self._last_reply)

    def _telemetry(self, sample: Telemetry, now: float) -> None:
        track = self._tracks.get(sample.sensor)
        if track is None:
            track = _Track(sample, now, self._capacity)
            self._tracks[sample.sensor] = track
        track.latest = sample
        track.history.append(sample)
        track.arrivals.append(now)
        track.count += 1
        self._prune(track, now)
        self._sequence(track, sample)

    def _sequence(self, track: _Track, sample: Telemetry) -> None:
        # The GIRV input report has no report ID, sequence or status field
        # (1000-3625 section 6.5.44.2, Figure 119), so the sequence and the
        # accuracy of the G line are placeholders. They stay out of the gap
        # statistics.
        if sample.kind is Kind.G:
            return
        previous = track.previous_seq
        track.previous_seq = sample.seq
        if previous is None:
            return
        if sample.seq == previous:
            track.repeats += 1
        else:
            track.missing += (sample.seq - previous - 1) % SEQUENCE_MODULUS

    def _restart_sequences(self) -> None:
        for track in self._tracks.values():
            track.previous_seq = None

    def _prune(self, track: _Track, now: float) -> None:
        while track.arrivals and track.arrivals[0] <= now - self._window:
            track.arrivals.popleft()

    def _rate(self, track: _Track, now: float) -> float:
        self._prune(track, now)
        elapsed = min(self._window, now - track.started)
        if elapsed <= 0:
            return 0.0
        return len(track.arrivals) / elapsed
