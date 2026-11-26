# SPDX-License-Identifier: MIT
import pytest

from pico_gui_bno085.model import DEFAULT_CAPACITY, Model
from pico_gui_bno085.protocol import (
    CalibrationStatus,
    Kind,
    RawResponse,
    Reply,
    Telemetry,
)
from pico_gui_bno085.stats import Timestamps


def sample(
    sensor: int = 5,
    seq: int = 0,
    kind: Kind = Kind.Q,
    time_us: int = 0,
    accuracy: int = 3,
) -> Telemetry:
    return Telemetry(kind, sensor, seq, time_us, accuracy, (0.0,))


def test_the_latest_sample_per_sensor_is_kept() -> None:
    model = Model()
    model.feed(sample(5, 0), "Q,5,0,0,3")
    model.feed(sample(8, 0, Kind.R), "R,8,0,0,3")
    model.feed(sample(5, 1, time_us=1_000), "Q,5,1,1000,3")
    stats = model.snapshot().sensors
    assert [s.sensor for s in stats] == [5, 8]
    assert stats[0].latest.seq == 1
    assert stats[0].figures.count == 2
    assert stats[1].figures.count == 1


def test_the_ring_buffer_keeps_the_newest_samples() -> None:
    model = Model(capacity=3)
    for seq in range(5):
        model.feed(sample(seq=seq, time_us=seq * 1_000), "")
    assert [s.seq for s in model.history(5)] == [2, 3, 4]
    assert model.history(99) == ()


def test_the_history_holds_five_seconds_at_the_fastest_plotted_rate() -> None:
    """The default capacity keeps the plots' five-second window.

    The motion plots draw the accelerometer, the one sensor of LP-8's
    table whose maximum reaches 500 Hz, so the capacity holds the window
    at every plotted sensor's maximum.
    """
    model = Model()
    for seq in range(DEFAULT_CAPACITY + 99):
        model.feed(sample(seq=seq % 256, time_us=seq * 2_000), "")
    history = model.history(5)
    assert len(history) == DEFAULT_CAPACITY
    assert history[0].time_us == 99 * 2_000
    assert history[-1].time_us == (DEFAULT_CAPACITY + 98) * 2_000


def test_the_rate_comes_from_the_event_times_not_the_arrivals() -> None:
    # A burst of four lines read in one chunk keeps the event-time rate.
    model = Model()
    for seq in range(4):
        model.feed(sample(seq=seq, time_us=seq * 2_500), "")
    assert model.snapshot().sensors[0].figures.rate_hz == 400.0


def test_a_sensor_that_stops_keeps_its_last_rate() -> None:
    model = Model()
    for seq in range(3):
        model.feed(sample(seq=seq, time_us=seq * 10_000), "")
    assert model.snapshot().sensors[0].figures.rate_hz == 100.0


def test_the_window_drops_the_samples_the_window_passes() -> None:
    model = Model(window_us=20_000)
    for seq in range(4):
        model.feed(sample(seq=seq, time_us=seq * 10_000), "")
    figures = model.snapshot().sensors[0].figures
    assert figures.count == 3
    assert figures.rate_hz == 100.0


def test_a_sequence_gap_counts_the_missing_reports() -> None:
    model = Model()
    for seq in (0, 1, 4, 5):
        model.feed(sample(seq=seq, time_us=seq * 1_000), "")
    assert model.snapshot().sensors[0].figures.missing == 2


def test_the_sequence_wraps_at_256_without_a_gap() -> None:
    model = Model()
    for index, seq in enumerate((254, 255, 0, 1)):
        model.feed(sample(seq=seq, time_us=index * 1_000), "")
    assert model.snapshot().sensors[0].figures.missing == 0


def test_a_gap_across_the_wrap_is_counted() -> None:
    model = Model()
    for index, seq in enumerate((254, 1)):
        model.feed(sample(seq=seq, time_us=index * 1_000), "")
    assert model.snapshot().sensors[0].figures.missing == 2


def test_a_repeated_sequence_is_a_repeat_and_not_a_gap() -> None:
    model = Model()
    for index, seq in enumerate((7, 7, 8)):
        model.feed(sample(seq=seq, time_us=index * 1_000), "")
    figures = model.snapshot().sensors[0].figures
    assert (figures.missing, figures.repeats) == (0, 1)


def test_girv_lines_have_no_sequence_and_are_not_gaps() -> None:
    model = Model()
    for index in range(10):
        model.feed(sample(42, 0, Kind.G, time_us=index * 1_000), "")
    figures = model.snapshot().sensors[0].figures
    assert (figures.missing, figures.repeats, figures.count) == (0, 0, 10)


def test_each_sensor_has_its_own_sequence() -> None:
    model = Model()
    model.feed(sample(5, 10), "")
    model.feed(sample(6, 200), "")
    model.feed(sample(5, 11), "")
    model.feed(sample(6, 201), "")
    assert [s.figures.missing for s in model.snapshot().sensors] == [0, 0]


def test_a_ready_reply_restarts_sequence_tracking() -> None:
    model = Model()
    model.feed(sample(seq=100), "")
    model.feed(Reply("ready", (), {}), "# ready")
    model.feed(sample(seq=3, time_us=1_000), "")
    assert model.snapshot().sensors[0].figures.missing == 0


def test_a_run_reply_restarts_sequence_tracking() -> None:
    model = Model()
    model.feed(Reply("run", ("1000",), {}), "# run 1000")
    model.feed(sample(seq=100), "")
    model.feed(Reply("done", (), {"events": "1"}), "# done events=1")
    model.feed(Reply("run", ("1000",), {}), "# run 1000")
    model.feed(sample(seq=7, time_us=1_000), "")
    assert model.snapshot().sensors[0].figures.missing == 0


def test_other_replies_do_not_restart_sequence_tracking() -> None:
    model = Model()
    model.feed(sample(seq=100), "")
    model.feed(Reply("done", (), {}), "")
    model.feed(Reply("ok", (), {}), "")
    model.feed(sample(seq=103, time_us=1_000), "")
    assert model.snapshot().sensors[0].figures.missing == 2


def test_a_backward_step_is_an_inversion() -> None:
    model = Model()
    model.feed(sample(seq=0, time_us=20_000), "")
    model.feed(sample(seq=1, time_us=19_700), "")
    figures = model.snapshot().sensors[0].figures
    assert figures.timestamps == Timestamps(False, 1, -300, 0.0)


def test_a_clock_fault_is_counted_and_a_ready_restart_is_not() -> None:
    model = Model()
    model.feed(sample(seq=0, time_us=20_000), "")
    model.feed(sample(seq=1, time_us=10_000), "")
    model.feed(Reply("ready", (), {}), "# ready")
    model.feed(sample(seq=0, time_us=1_000), "")
    figures = model.snapshot().sensors[0].figures
    assert figures.clock_faults == 1
    assert figures.count == 1


def test_the_commanded_interval_is_the_nominal_when_one_was_sent() -> None:
    model = Model()
    model.feed(sample(time_us=0), "")
    model.feed(sample(seq=1, time_us=9_000), "")
    model.feed(sample(seq=2, time_us=18_000), "")
    figures = model.snapshot({5: 10_000}).sensors[0].figures
    assert (figures.nominal_interval_us, figures.nominal_is_commanded) == (
        10_000,
        True,
    )
    without = model.snapshot().sensors[0].figures
    assert (without.nominal_interval_us, without.nominal_is_commanded) == (
        9_000,
        False,
    )


def test_replies_rejected_lines_and_the_last_reply_are_counted() -> None:
    model = Model()
    model.feed(Reply("ok", (), {}), "# ok")
    cal_line = "# cal status=0 accel=1 gyro=0 mag=1 planar=0 table=0"
    model.feed(CalibrationStatus(0, 1, 0, 1, 0, 0), cal_line)
    model.reject()
    snapshot = model.snapshot()
    assert (snapshot.replies, snapshot.rejected) == (2, 1)
    assert snapshot.last_reply == Reply("ok", (), {})
    assert snapshot.calibration == CalibrationStatus(0, 1, 0, 1, 0, 0)
    assert snapshot.response_line == cal_line


def test_a_raw_response_line_is_kept_as_received() -> None:
    model = Model()
    line = "# resp cmd=03 seq=9 rseq=1 r=0000000000000000000000"
    model.feed(
        RawResponse(0x03, 9, 1, bytes(11)),
        line,
    )
    assert model.snapshot().response_line == line
    assert model.snapshot().calibration is None


def test_a_model_needs_a_positive_capacity_and_window() -> None:
    with pytest.raises(ValueError, match="positive"):
        Model(capacity=0)
    with pytest.raises(ValueError, match="positive"):
        Model(window_us=0)
