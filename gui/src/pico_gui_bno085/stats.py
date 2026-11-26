# SPDX-License-Identifier: MIT
"""Per-sensor stream figures over a rolling window of event time.

Every figure comes from the event times the lines carry (`event_us` of
LP-4), not from the moments the host read them, so a burst of lines in one
USB chunk leaves the rate and the jitter unchanged. The formulas match the
bench harness over one run's events: the rate is the count of intervals
divided by the span between the first and the last event time, and the
99th-percentile jitter is taken the same way. The window is event time, so
a sensor that stops delivering keeps its last figures until newer events
push the old ones out.

The gap count differs from the bench's by design: the bench's counter takes
a repeated sequence number for 255 lost reports and counts the G line, whose
sequence is a placeholder (1000-3625 §6.5.44.2). This module counts missing
and repeated reports separately, leaves the G line out, and compares
nothing across a `# ready` or `# run` boundary, where the hub may have reset
its counters.
"""

import itertools
from collections import deque
from collections.abc import Sequence
from dataclasses import dataclass

from pico_gui_bno085.protocol import RESTART_STEP_US, SEQUENCE_MODULUS

DEFAULT_WINDOW_US = 2_000_000

__all__ = (
    "DEFAULT_WINDOW_US",
    "Figures",
    "Sample",
    "StreamWindow",
    "Timestamps",
    "delivered_rate_hz",
    "figures_of",
    "median_interval",
    "sequence_gaps",
    "timestamp_detail",
)


@dataclass(frozen=True)
class Sample:
    """One telemetry line reduced to what the figures need.

    `sequenced` is false for the G line, whose report carries a placeholder
    sequence and accuracy (1000-3625 §6.5.44.2). `restart` marks the first
    sample after a `# ready` or `# run` line.
    """

    time_us: int
    seq: int
    sequenced: bool
    restart: bool


@dataclass(frozen=True)
class Timestamps:
    """The event-time order of one sensor's retained samples."""

    monotonic: bool
    inversions: int
    worst_backward_us: int
    p99_ratio: float


@dataclass(frozen=True)
class Figures:
    """The stream figures of one sensor over its retained samples."""

    sensor: int
    count: int
    rate_hz: float
    missing: int
    repeats: int
    timestamps: Timestamps
    nominal_interval_us: int
    nominal_is_commanded: bool
    clock_faults: int


def delivered_rate_hz(times_us: Sequence[int]) -> float:
    """Return the delivered rate of one sensor's event times.

    The count of intervals, not of events, over the span between the first
    and the last time: `(n - 1) * 1e6 / span`. Fewer than two samples, or a
    span that is not positive, gives zero.
    """
    if len(times_us) < 2:
        return 0.0
    span_us = times_us[-1] - times_us[0]
    if span_us <= 0:
        return 0.0
    return (len(times_us) - 1) * 1_000_000.0 / span_us


def median_interval(times_us: Sequence[int]) -> int:
    """Return the lower middle of the sorted observed intervals.

    An even count takes the lower of its two middle values, so the nominal
    interval stays a whole number of microseconds.
    """
    ordered = sorted(b - a for a, b in itertools.pairwise(times_us))
    if not ordered:
        return 0
    return ordered[(len(ordered) - 1) // 2]


def sequence_gaps(samples: Sequence[Sample]) -> tuple[int, int]:
    """Return the missing and the repeated report counts of one sensor.

    A sequence number that repeats the previous one counts as a repeat. A
    forward jump counts the numbers it skipped, wrapping at 256. Samples
    without a sequence, and the first sample after a restart boundary, are
    compared with nothing.
    """
    missing = 0
    repeats = 0
    previous: int | None = None
    for sample in samples:
        if not sample.sequenced:
            continue
        if sample.restart:
            previous = None
        if previous is not None:
            if sample.seq == previous:
                repeats += 1
            else:
                missing += (sample.seq - previous - 1) % SEQUENCE_MODULUS
        previous = sample.seq
    return missing, repeats


def timestamp_detail(times_us: Sequence[int], nominal_us: int) -> Timestamps:
    """Return the order figures of one sensor's event times.

    An inversion is an interval that is zero or negative. `p99_ratio` is the
    99th-percentile interval as a fraction of `nominal_us`, taken as the
    absolute distance from it; a non-positive nominal interval, or fewer than
    two samples, gives zero. `worst_backward_us` is the most negative
    interval, or zero when every interval runs forward.
    """
    deltas = [b - a for a, b in itertools.pairwise(times_us)]
    backward = [delta for delta in deltas if delta <= 0]
    ratio = 0.0
    if deltas and nominal_us > 0:
        ordered = sorted(deltas)
        p99 = ordered[min(len(ordered) - 1, int(0.99 * len(ordered)))]
        ratio = abs(p99 - nominal_us) / nominal_us
    return Timestamps(
        not backward,
        len(backward),
        min(backward, default=0),
        ratio,
    )


def figures_of(
    sensor: int,
    samples: Sequence[Sample],
    commanded_us: int | None,
    clock_faults: int = 0,
) -> Figures:
    """Return every figure of one sensor over `samples`.

    `clock_faults` is the count the caller kept of unflagged backward steps
    of `RESTART_STEP_US` or more, which the samples no longer show.

    The nominal interval is `commanded_us` when the session sent a `cfg` for
    the sensor, and otherwise the median observed interval. The caller
    states which basis it used through `nominal_is_commanded`.
    """
    times_us = [sample.time_us for sample in samples]
    missing, repeats = sequence_gaps(samples)
    nominal_us = commanded_us if commanded_us is not None else median_interval(times_us)
    return Figures(
        sensor,
        len(samples),
        delivered_rate_hz(times_us),
        missing,
        repeats,
        timestamp_detail(times_us, nominal_us),
        nominal_us,
        commanded_us is not None,
        clock_faults,
    )


class StreamWindow:
    """The retained samples of one sensor within a window of event time.

    The window ends at the newest event time, so its length is a span of the
    board's clock rather than of the host's. An event time that steps back by
    `RESTART_STEP_US` or more starts the window afresh with that sample, so
    it never holds the older, larger times beside the new ones. If the sample
    is flagged as a restart (the first after a `# ready` or `# run` line)
    the step is expected and not counted; otherwise it is a reconstruction
    fault (PR-3) and adds one to a count that outlives the window. A
    smaller step is kept as an inversion. A flagged restart whose time
    continues forward leaves the window as it is.
    """

    def __init__(self, window_us: int = DEFAULT_WINDOW_US) -> None:
        if window_us <= 0:
            raise ValueError("the window must be positive")
        self._window_us = window_us
        self._samples: deque[Sample] = deque()
        self._clock_faults = 0
        self._cache: tuple[int | None, Figures] | None = None

    def add(self, sample: Sample) -> None:
        """Retain `sample` and drop those older than the window allows."""
        if (
            self._samples
            and self._samples[-1].time_us - sample.time_us >= RESTART_STEP_US
        ):
            if not sample.restart:
                self._clock_faults += 1
            self._samples.clear()
        self._samples.append(sample)
        edge = sample.time_us - self._window_us
        while self._samples and self._samples[0].time_us < edge:
            self._samples.popleft()
        self._cache = None

    def figures(self, sensor: int, commanded_us: int | None) -> Figures:
        """Return the figures over the retained samples, computed once."""
        if self._cache is None or self._cache[0] != commanded_us:
            self._cache = (
                commanded_us,
                figures_of(
                    sensor,
                    self._samples,
                    commanded_us,
                    self._clock_faults,
                ),
            )
        return self._cache[1]
