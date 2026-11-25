# SPDX-License-Identifier: MIT
import pytest

from pico_gui_bno085.model import Model
from pico_gui_bno085.protocol import Kind, Reply, SaveDcdStatus, Telemetry


def sample(
    sensor: int = 5,
    seq: int = 0,
    kind: Kind = Kind.Q,
    time_us: int = 0,
) -> Telemetry:
    return Telemetry(kind, sensor, seq, time_us, 3, (0.0,))


def test_the_latest_sample_per_sensor_is_kept() -> None:
    model = Model()
    model.feed(sample(5, 0), 0.0)
    model.feed(sample(8, 0, Kind.R), 0.0)
    model.feed(sample(5, 1), 0.1)
    stats = model.snapshot(0.1).sensors
    assert [s.sensor for s in stats] == [5, 8]
    assert stats[0].latest.seq == 1
    assert stats[0].count == 2
    assert stats[1].count == 1


def test_the_ring_buffer_keeps_the_newest_samples() -> None:
    model = Model(capacity=3)
    for seq in range(5):
        model.feed(sample(seq=seq), float(seq))
    assert [s.seq for s in model.history(5)] == [2, 3, 4]
    assert model.history(99) == ()


def test_the_rate_is_the_arrivals_in_the_window_over_the_window() -> None:
    model = Model(window_seconds=1.0)
    for n in range(200):
        model.feed(sample(seq=n % 256), 10.0 + n * 0.01)
    assert model.snapshot(12.0).sensors[0].rate_hz == pytest.approx(100.0, abs=1.5)


def test_the_rate_before_a_full_window_uses_the_elapsed_time() -> None:
    model = Model(window_seconds=1.0)
    for n in range(11):
        model.feed(sample(seq=n), 5.0 + n * 0.01)
    assert model.snapshot(5.1).sensors[0].rate_hz == pytest.approx(11 / 0.1)


def test_the_rate_falls_to_zero_when_the_stream_stops() -> None:
    model = Model(window_seconds=1.0)
    model.feed(sample(seq=0), 0.0)
    model.feed(sample(seq=1), 0.5)
    assert model.snapshot(0.6).sensors[0].rate_hz > 0
    assert model.snapshot(5.0).sensors[0].rate_hz == 0.0


def test_a_sequence_gap_counts_the_missing_reports() -> None:
    model = Model()
    for seq in (0, 1, 4, 5):
        model.feed(sample(seq=seq), 0.0)
    assert model.snapshot(0.0).sensors[0].missing == 2


def test_the_sequence_wraps_at_256_without_a_gap() -> None:
    model = Model()
    for seq in (254, 255, 0, 1):
        model.feed(sample(seq=seq), 0.0)
    assert model.snapshot(0.0).sensors[0].missing == 0


def test_a_gap_across_the_wrap_is_counted() -> None:
    model = Model()
    for seq in (254, 1):
        model.feed(sample(seq=seq), 0.0)
    assert model.snapshot(0.0).sensors[0].missing == 2


def test_a_repeated_sequence_is_a_repeat_and_not_a_gap() -> None:
    model = Model()
    for seq in (7, 7, 8):
        model.feed(sample(seq=seq), 0.0)
    stats = model.snapshot(0.0).sensors[0]
    assert (stats.missing, stats.repeats) == (0, 1)


def test_girv_lines_have_no_sequence_and_are_not_gaps() -> None:
    model = Model()
    for _ in range(10):
        model.feed(sample(42, 0, Kind.G), 0.0)
    stats = model.snapshot(0.0).sensors[0]
    assert (stats.missing, stats.repeats, stats.count) == (0, 0, 10)


def test_each_sensor_has_its_own_sequence() -> None:
    model = Model()
    model.feed(sample(5, 10), 0.0)
    model.feed(sample(6, 200), 0.0)
    model.feed(sample(5, 11), 0.0)
    model.feed(sample(6, 201), 0.0)
    assert [s.missing for s in model.snapshot(0.0).sensors] == [0, 0]


def test_a_ready_reply_restarts_sequence_tracking() -> None:
    model = Model()
    model.feed(sample(seq=100), 0.0)
    model.feed(Reply("ready", (), {}), 0.1)
    model.feed(sample(seq=3), 0.2)
    assert model.snapshot(0.2).sensors[0].missing == 0


def test_a_run_reply_restarts_sequence_tracking() -> None:
    model = Model()
    model.feed(Reply("run", ("1000",), {}), 0.0)
    model.feed(sample(seq=100), 0.1)
    model.feed(sample(seq=101), 0.2)
    model.feed(Reply("done", (), {"events": "2"}), 0.3)
    model.feed(Reply("run", ("1000",), {}), 5.0)
    model.feed(sample(seq=7), 5.1)
    assert model.snapshot(5.1).sensors[0].missing == 0


def test_a_gap_inside_a_run_is_still_counted() -> None:
    model = Model()
    model.feed(Reply("run", ("1000",), {}), 0.0)
    for seq in (100, 103):
        model.feed(sample(seq=seq), 0.1)
    assert model.snapshot(0.1).sensors[0].missing == 2


def test_other_replies_do_not_restart_sequence_tracking() -> None:
    model = Model()
    model.feed(sample(seq=100), 0.0)
    model.feed(Reply("done", (), {}), 0.1)
    model.feed(Reply("ok", (), {}), 0.1)
    model.feed(sample(seq=103), 0.2)
    assert model.snapshot(0.2).sensors[0].missing == 2


def test_replies_and_rejected_lines_are_counted() -> None:
    model = Model()
    model.feed(Reply("ok", (), {}), 0.0)
    model.feed(SaveDcdStatus(0), 0.0)
    model.reject()
    snapshot = model.snapshot(0.0)
    assert (snapshot.replies, snapshot.rejected) == (2, 1)
    assert snapshot.last_reply == Reply("ok", (), {})


def test_a_model_needs_a_positive_capacity_and_window() -> None:
    with pytest.raises(ValueError, match="positive"):
        Model(capacity=0)
    with pytest.raises(ValueError, match="positive"):
        Model(window_seconds=0)
