# SPDX-License-Identifier: MIT
"""The motion plots' series: window, decimation and points."""

import itertools

import pytest

from pico_gui_bno085.charts import (
    MAX_POINTS,
    WINDOW_SECONDS,
    decimated,
    latest_window,
    motion_plot,
)
from pico_gui_bno085.protocol import Kind, Telemetry


def vector(
    sensor: int,
    seq: int,
    time_us: int,
    values: tuple[float, float, float],
) -> Telemetry:
    """Return one `V` line's sample (LP-6)."""
    return Telemetry(Kind.V, sensor, seq, time_us, 2, values)


def legacy(sensor: int, seq: int, time_us: int) -> Telemetry:
    """Return one `E` line's sample (LP-4): no payload, no values."""
    return Telemetry(Kind.E, sensor, seq, time_us, 3, ())


def samples(count: int, step_us: int, start_us: int = 0) -> tuple[Telemetry, ...]:
    """Return `count` vector samples `step_us` apart in event time."""
    return tuple(
        vector(1, index % 256, start_us + index * step_us, (0.5, -0.25, 9.8))
        for index in range(count)
    )


def test_the_window_keeps_the_latest_five_seconds_of_event_time() -> None:
    # Eleven seconds of samples at ten a second: the newest is 11.0 s, so
    # the window starts at 6.0 s and keeps 51 samples.
    history = samples(111, 100_000)
    window = latest_window(history, WINDOW_SECONDS)
    assert window[0].time_us == 6_000_000
    assert window[-1].time_us == 11_000_000
    assert len(window) == 51


def test_the_window_of_no_samples_is_empty() -> None:
    assert latest_window((), WINDOW_SECONDS) == ()


def test_a_short_history_is_undecimated() -> None:
    history = samples(5, 1_000)
    assert decimated(history) == history


def test_decimation_keeps_the_newest_and_stays_within_the_limit() -> None:
    history = samples(2_501, 2_000)
    kept = decimated(history)
    assert len(kept) <= MAX_POINTS
    assert kept[-1] is history[-1]
    # The stride is even, so no kept sample is adjacent to another.
    gaps = {
        later.time_us - earlier.time_us for earlier, later in itertools.pairwise(kept)
    }
    assert min(gaps) == 9 * 2_000


def test_decimation_of_a_set_just_above_the_limit_keeps_both_ends() -> None:
    history = samples(MAX_POINTS + 1, 1_000)
    kept = decimated(history)
    assert len(kept) <= MAX_POINTS
    assert kept[0] is history[0]
    assert kept[-1] is history[-1]


def test_a_decimation_limit_below_one_is_refused() -> None:
    with pytest.raises(ValueError, match="positive"):
        decimated(samples(4, 1_000), limit=0)


def test_the_plot_carries_one_point_series_per_axis() -> None:
    history = samples(3, 500_000)
    motion = motion_plot(history)
    assert motion is not None
    assert motion.newest_s == pytest.approx(1.0)
    for axis in range(3):
        series = motion.points[axis]
        assert [time_s for time_s, _ in series] == [0.0, 0.5, 1.0]
        assert [value for _, value in series] == [
            pytest.approx((0.5, -0.25, 9.8)[axis]),
        ] * 3


def test_the_plot_of_an_empty_history_is_none() -> None:
    assert motion_plot(()) is None


@pytest.mark.parametrize("sensor", [0x01, 0x02, 0x03])
def test_a_legacy_e_line_is_left_out_of_the_plot(sensor: int) -> None:
    """An E line carries no values (LP-4, LP-5), so the plot skips it."""
    history = (
        vector(sensor, 0, 1_000_000, (0.5, -0.25, 9.8)),
        legacy(sensor, 1, 1_002_000),
    )
    motion = motion_plot(history)
    assert motion is not None
    assert motion.newest_s == pytest.approx(1.0)
    assert motion.points[0] == ((1.0, 0.5),)


def test_a_history_of_e_lines_alone_has_no_plot() -> None:
    history = tuple(legacy(1, index, index * 1_000) for index in range(3))
    assert motion_plot(history) is None


def test_the_plot_starts_a_fresh_window_when_the_clock_steps_back() -> None:
    """The window keeps only the samples after the last backwards step.

    The probe: ten samples at 9.000 to 9.018 s, a `# ready` restart,
    then three at 0.001 to 0.005 s. A window measured from the newest
    arrival would keep the nine-second samples beside the new ones.
    """
    reading = (0.5, -0.25, 9.8)
    history = tuple(
        vector(1, index, round((9.0 + index * 0.002) * 1_000_000), reading)
        for index in range(10)
    ) + tuple(
        vector(1, 10 + index, round((0.001 + index * 0.002) * 1_000_000), reading)
        for index in range(3)
    )
    motion = motion_plot(history)
    assert motion is not None
    assert motion.newest_s == pytest.approx(0.005)
    assert [time_s for time_s, _ in motion.points[0]] == [0.001, 0.003, 0.005]


def test_a_backward_step_within_the_quantisation_bound_keeps_the_window() -> None:
    """PR-3 allows a backward step under 400 µs; it is not a restart.

    The probe: one hundred samples 2 ms apart, then one 399 µs before
    its predecessor. The window keeps all one hundred and one.
    """
    reading = (0.5, -0.25, 9.8)
    history = tuple(
        vector(1, index, 1_000_000 + index * 2000, reading) for index in range(100)
    )
    history += (vector(1, 100, history[-1].time_us - 399, reading),)
    motion = motion_plot(history)
    assert motion is not None
    assert len(motion.points[0]) == 101


def test_a_backward_step_of_four_hundred_microseconds_is_a_restart() -> None:
    reading = (0.5, -0.25, 9.8)
    history = tuple(
        vector(1, index, 1_000_000 + index * 2000, reading) for index in range(100)
    )
    history += (vector(1, 100, history[-1].time_us - 400, reading),)
    motion = motion_plot(history)
    assert motion is not None
    assert len(motion.points[0]) == 1


def test_the_plot_decimates_its_window() -> None:
    # A 25-second history at 500 Hz: the window keeps the last 2501
    # samples and the series thin them.
    history = samples(12_501, 2_000)
    motion = motion_plot(history)
    assert motion is not None
    assert motion.newest_s == pytest.approx(25.0)
    for axis in range(3):
        assert len(motion.points[axis]) <= MAX_POINTS
        newest = motion.points[axis][-1]
        assert newest[0] == pytest.approx(25.0)
        assert newest[1] == pytest.approx((0.5, -0.25, 9.8)[axis])
