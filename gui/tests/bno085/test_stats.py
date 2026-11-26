# SPDX-License-Identifier: MIT
import pytest

from pico_gui_bno085.stats import (
    Sample,
    StreamWindow,
    delivered_rate_hz,
    figures_of,
    median_interval,
    sequence_gaps,
    timestamp_detail,
)


def sample(
    time_us: int,
    seq: int,
    *,
    sequenced: bool = True,
    restart: bool = False,
) -> Sample:
    return Sample(time_us, seq, sequenced, restart)


def test_the_rate_counts_intervals_over_the_span() -> None:
    times = [0, 10_000, 20_000, 30_000]
    assert delivered_rate_hz(times) == 100.0


def test_the_rate_of_a_burst_keeps_its_event_time() -> None:
    # A chunk that delivers four events at once still shows their true rate.
    assert delivered_rate_hz([0, 2_500, 5_000, 7_500]) == 400.0


def test_one_sample_or_a_still_span_gives_no_rate() -> None:
    assert delivered_rate_hz([5_000]) == 0.0
    assert delivered_rate_hz([5_000, 5_000]) == 0.0
    assert delivered_rate_hz([5_000, 4_000]) == 0.0


def test_the_median_takes_the_lower_of_two_middle_values() -> None:
    assert median_interval([0, 10_000, 20_000]) == 10_000
    assert median_interval([0, 10_000, 20_000, 40_000]) == 10_000


def test_the_median_of_one_sample_is_zero() -> None:
    assert median_interval([5_000]) == 0


def test_a_forward_jump_counts_the_reports_it_skipped() -> None:
    samples = [sample(0, 0), sample(10_000, 4)]
    assert sequence_gaps(samples) == (3, 0)


def test_a_repeated_sequence_is_a_repeat_and_not_a_loss() -> None:
    samples = [sample(0, 7), sample(10_000, 7), sample(20_000, 8)]
    assert sequence_gaps(samples) == (0, 1)


def test_the_sequence_wraps_at_256_without_a_gap() -> None:
    samples = [sample(0, 254), sample(10_000, 255), sample(20_000, 0)]
    assert sequence_gaps(samples) == (0, 0)


def test_a_restart_boundary_compares_nothing() -> None:
    samples = [sample(0, 100), sample(10_000, 3, restart=True)]
    assert sequence_gaps(samples) == (0, 0)


def test_the_g_line_is_not_a_gap_source() -> None:
    samples = [sample(0, 0), sample(10_000, 0, sequenced=False), sample(20_000, 1)]
    assert sequence_gaps(samples) == (0, 0)


def test_the_worst_backward_step_and_the_inversions() -> None:
    detail = timestamp_detail([0, 10_000, 5_000, 30_000], 10_000)
    assert detail.monotonic is False
    assert detail.inversions == 1
    assert detail.worst_backward_us == -5_000


def test_a_equal_step_is_an_inversion() -> None:
    detail = timestamp_detail([0, 10_000, 10_000], 10_000)
    assert (detail.monotonic, detail.inversions) == (False, 1)


def test_forward_steps_leave_the_worst_at_zero() -> None:
    detail = timestamp_detail([0, 10_000, 20_000], 10_000)
    assert (detail.monotonic, detail.inversions, detail.worst_backward_us) == (
        True,
        0,
        0,
    )


def test_the_p99_jitter_is_the_distance_from_the_nominal_interval() -> None:
    detail = timestamp_detail([0, 10_000, 20_000, 35_000], 10_000)
    assert detail.p99_ratio == 0.5


def test_a_non_positive_nominal_interval_gives_no_jitter() -> None:
    assert timestamp_detail([0, 10_000], 0).p99_ratio == 0.0


def test_the_figures_take_the_commanded_interval_as_their_nominal() -> None:
    samples = [sample(0, 0), sample(10_000, 1), sample(20_000, 2)]
    figures = figures_of(5, samples, 10_000)
    assert (figures.nominal_interval_us, figures.nominal_is_commanded) == (
        10_000,
        True,
    )


def test_the_figures_take_the_median_when_no_interval_was_commanded() -> None:
    samples = [sample(0, 0), sample(10_000, 1), sample(22_000, 2)]
    figures = figures_of(5, samples, None)
    assert (figures.nominal_interval_us, figures.nominal_is_commanded) == (
        10_000,
        False,
    )


def test_a_window_drops_samples_older_than_its_span() -> None:
    window = StreamWindow(window_us=20_000)
    for time_us in (0, 10_000, 20_000, 30_000):
        window.add(sample(time_us, 0))
    figures = window.figures(5, None)
    assert figures.count == 3
    assert figures.rate_hz == delivered_rate_hz([10_000, 20_000, 30_000])


def test_a_large_step_back_starts_the_window_afresh() -> None:
    window = StreamWindow(window_us=1_000_000)
    for index in range(200):
        window.add(sample(3_600_000_000 + index * 2_000, index % 256))
    # The clock restarts at zero; the old times are all larger than any new
    # one, so a window that only prunes from the older end would keep them.
    for index in range(2_000):
        window.add(sample(index * 2_000, index % 256))
    figures = window.figures(5, None)
    assert figures.count == 501
    assert figures.timestamps.inversions == 0


def test_a_step_back_under_the_restart_bound_is_kept() -> None:
    window = StreamWindow(window_us=1_000_000)
    for time_us in (10_000, 20_000, 19_601, 30_000):
        window.add(sample(time_us, 0))
    figures = window.figures(5, None)
    assert figures.count == 4
    assert figures.timestamps.inversions == 1


def test_a_step_back_of_the_restart_bound_starts_afresh() -> None:
    window = StreamWindow(window_us=1_000_000)
    for time_us in (10_000, 20_000, 19_600, 30_000):
        window.add(sample(time_us, 0))
    assert window.figures(5, None).count == 2


def test_a_flagged_restart_starts_afresh_and_is_not_a_fault() -> None:
    window = StreamWindow(window_us=1_000_000)
    for time_us in (10_000, 20_000):
        window.add(sample(time_us, 0))
    window.add(sample(1_000, 0, restart=True))
    window.add(sample(11_000, 1))
    figures = window.figures(5, None)
    assert figures.clock_faults == 0
    assert figures.count == 2


def test_a_flagged_restart_that_continues_forward_keeps_the_window() -> None:
    window = StreamWindow(window_us=1_000_000)
    window.add(sample(10_000, 0))
    window.add(sample(20_000, 1, restart=True))
    assert window.figures(5, None).count == 2


def test_an_unflagged_step_of_ten_milliseconds_is_one_fault() -> None:
    window = StreamWindow(window_us=1_000_000)
    for time_us in (20_000, 30_000, 20_000, 30_000):
        window.add(sample(time_us, 0))
    figures = window.figures(5, None)
    assert figures.clock_faults == 1
    assert figures.count == 2


def test_each_unflagged_step_adds_one_fault_and_the_count_survives() -> None:
    window = StreamWindow(window_us=1_000_000)
    for time_us in (50_000, 40_000, 30_000, 31_000, 32_000):
        window.add(sample(time_us, 0))
    figures = window.figures(5, None)
    assert figures.clock_faults == 2
    assert figures.count == 3


def test_a_step_of_399_microseconds_is_an_inversion_and_not_a_fault() -> None:
    window = StreamWindow(window_us=1_000_000)
    for time_us in (20_000, 19_601):
        window.add(sample(time_us, 0))
    figures = window.figures(5, None)
    assert (figures.clock_faults, figures.timestamps.inversions) == (0, 1)


def test_a_window_needs_a_positive_span() -> None:
    with pytest.raises(ValueError, match="positive"):
        StreamWindow(window_us=0)


def test_a_window_recomputes_only_when_it_must() -> None:
    window = StreamWindow(window_us=1_000_000)
    window.add(sample(0, 0))
    first = window.figures(5, 10_000)
    window.add(sample(10_000, 1))
    second = window.figures(5, 10_000)
    assert first is not second
    assert window.figures(5, 10_000) is second
    assert window.figures(5, 20_000) is not second
