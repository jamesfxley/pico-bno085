# SPDX-License-Identifier: MIT
"""The motion plots' series, built from the model's retained samples.

Each plot draws one calibrated vector sensor of LP-6: the x, y and z of
its `V` lines against its event time in seconds, over the latest five
seconds of that sensor's own clock. The series are thinned to at most
`MAX_POINTS` points with the newest kept, so a refresh never sends more
than that per axis, however fast the sensor reports.
"""

from collections.abc import Mapping, Sequence
from dataclasses import dataclass

from pico_gui_bno085.protocol import RESTART_STEP_US, Kind, Telemetry

WINDOW_SECONDS = 5.0
MAX_POINTS = 300


@dataclass(frozen=True)
class PlotSpec:
    """One plotted sensor: its label and the unit of its values (LP-6)."""

    label: str
    unit: str


# LP-6: `V` carries acceleration including gravity (`01`) in m/s²,
# calibrated angular velocity (`02`) in rad/s and the calibrated magnetic
# field (`03`) in µT.
MOTION_PLOTS: Mapping[int, PlotSpec] = {
    0x01: PlotSpec("Acceleration", "m/s²"),
    0x02: PlotSpec("Angular velocity", "rad/s"),
    0x03: PlotSpec("Magnetic field", "µT"),
}

__all__ = (
    "MAX_POINTS",
    "MOTION_PLOTS",
    "WINDOW_SECONDS",
    "Motion",
    "PlotSpec",
    "decimated",
    "latest_window",
    "motion_plot",
)


@dataclass(frozen=True)
class Motion:
    """One sensor's thinned series, in seconds of event time.

    `points` holds one entry per axis, each a pair of the event time in
    seconds and the value, oldest first.
    """

    newest_s: float
    points: tuple[tuple[tuple[float, float], ...], ...]


def latest_window(
    samples: Sequence[Telemetry],
    seconds: float,
) -> tuple[Telemetry, ...]:
    """Return the samples within `seconds` of the newest event time."""
    if not samples:
        return ()
    edge = samples[-1].time_us - round(seconds * 1_000_000)
    return tuple(sample for sample in samples if sample.time_us >= edge)


def decimated(
    samples: Sequence[Telemetry],
    limit: int = MAX_POINTS,
) -> tuple[Telemetry, ...]:
    """Return `samples` thinned to at most `limit`, the newest always kept.

    The stride runs from the newest sample backwards, so the newest is
    kept and the kept samples are evenly spaced over the window.
    """
    if limit < 1:
        raise ValueError("the limit must be positive")
    if len(samples) <= limit:
        return tuple(samples)
    stride = -(-len(samples) // limit)
    return tuple(samples[::-1][::stride][::-1])


def _monotonic_tail(samples: Sequence[Telemetry]) -> tuple[Telemetry, ...]:
    """Return the samples after the last restart of the event time.

    An event time can step back — a `# ready` or `# run` restart, or the
    board's clock restarting — and a window measured from the newest
    arrival would hold the older, larger times beside the new ones as
    long as the history retains both. PR-3 allows a backward step under
    `RESTART_STEP_US` from the wire format's quantisation, so only a
    step of that size or more counts as a restart.
    """
    if not samples:
        return ()
    start = 0
    for index in range(1, len(samples)):
        step = samples[index - 1].time_us - samples[index].time_us
        if step >= RESTART_STEP_US:
            start = index
    return tuple(samples[start:])


def motion_plot(history: Sequence[Telemetry]) -> Motion | None:
    """Return one sensor's plot over its latest five seconds, or none.

    Only the sensor's `V` lines carry the x, y and z the plot draws
    (LP-6); a legacy E line, printed while payload mode is off (LP-4,
    LP-5), has no values and is left out. The window holds the samples
    after the last backward step of `RESTART_STEP_US` (400 µs) or more,
    so a restart that steps the clock back starts a fresh window. None is
    a sensor whose retained samples hold no payload line.
    """
    vectors = [sample for sample in history if sample.kind is Kind.V]
    window = latest_window(_monotonic_tail(vectors), WINDOW_SECONDS)
    if not window:
        return None
    kept = decimated(window)
    return Motion(
        kept[-1].time_us / 1_000_000,
        tuple(
            tuple((sample.time_us / 1_000_000, sample.values[axis]) for sample in kept)
            for axis in range(3)
        ),
    )
