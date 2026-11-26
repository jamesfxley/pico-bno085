# SPDX-License-Identifier: MIT
import math
from typing import cast

import pytest

from pico_gui.commands import CommandError
from pico_gui_bno085.charts import Motion
from pico_gui_bno085.controller import ControllerState, Phase
from pico_gui_bno085.model import SensorStats, Snapshot
from pico_gui_bno085.orientation import orientation_of
from pico_gui_bno085.panel import (
    ACCURACY_NAMES,
    HEALTH_COLUMNS,
    NO_BOARD,
    accuracy_text,
    calibration_text,
    commanded_hz,
    controller_text,
    convention_text,
    euler_label,
    health_rows,
    interval_us,
    missing_rotation_text,
    motion_options,
    motion_window_text,
    no_motion_text,
    progress_text,
    recording_text,
    reference_text,
    response_text,
    rotation_options,
    sensor_name,
    sensor_options,
    sensor_rows,
    summary,
)
from pico_gui_bno085.protocol import CalibrationStatus, Kind, Telemetry
from pico_gui_bno085.stats import Figures, Timestamps


def stats(
    sensor: int = 5,
    kind: Kind = Kind.Q,
    accuracy: int = 3,
    *,
    commanded: bool = False,
) -> SensorStats:
    latest = Telemetry(kind, sensor, 0, 0, accuracy, (0.0,) * 5)
    figures = Figures(
        sensor,
        12,
        100.0,
        3,
        1,
        Timestamps(True, 0, 0, 0.25),
        10_000,
        commanded,
        2,
    )
    return SensorStats(sensor, latest, figures)


def snapshot(*sensors: SensorStats) -> Snapshot:
    return Snapshot(sensors, 4, 2, None, None, "")


def test_a_known_sensor_has_a_name_and_its_id() -> None:
    assert sensor_name(0x2A) == "Gyro-integrated rotation vector (0x2A)"


def test_an_unknown_sensor_is_still_shown() -> None:
    assert sensor_name(0x7E) == "Sensor (0x7E)"


def test_the_select_offers_every_sensor_with_its_maximum_rate() -> None:
    options = sensor_options()
    assert len(options) == 13
    assert "at most 500 Hz" in options[0x01]
    assert "at most 1000 Hz" in options[0x2A]


def test_the_select_marks_the_two_unconfirmed_rates() -> None:
    options = sensor_options()
    assert "rate unconfirmed" in options[0x07]
    assert "rate unconfirmed" in options[0x0F]
    assert "rate unconfirmed" not in options[0x02]


def test_the_interval_input_keeps_whole_microseconds() -> None:
    assert interval_us(10_000.0) == 10_000
    assert interval_us(1) == 1


def test_the_interval_input_refuses_what_the_bench_would_refuse() -> None:
    with pytest.raises(CommandError, match="empty"):
        interval_us(None)
    for bad in (0.0, -5.0, 2.5, float("nan"), float("inf"), float("-inf")):
        with pytest.raises(CommandError, match="whole number"):
            interval_us(bad)


def test_the_accuracy_shows_its_lp4_name() -> None:
    names = [accuracy_text(stats(accuracy=value)) for value in range(4)]
    assert names == list(ACCURACY_NAMES)


def test_the_g_line_reports_no_accuracy() -> None:
    assert accuracy_text(stats(kind=Kind.G)) == "not reported"


def test_the_sensors_table_shows_the_commanded_rate_beside_the_delivered_one() -> None:
    rows = sensor_rows(
        snapshot(stats(5), stats(8, kind=Kind.R)),
        enabled={5},
        commanded={5: 5_000},
    )
    assert rows[0] == {
        "sensor": "Rotation vector (0x05)",
        "state": "enabled",
        "commanded": "200 Hz",
        "rate": "100.0",
        "count": 12,
        "accuracy": "high",
    }
    assert rows[1]["state"] == "off"
    assert rows[1]["commanded"] == "—"


def test_the_health_table_shows_gaps_jitter_and_the_nominal_basis() -> None:
    rows = health_rows(snapshot(stats(5, commanded=True)))
    assert rows[0] == {
        "sensor": "Rotation vector (0x05)",
        "rate": "100.0",
        "missing": 3,
        "repeats": 1,
        "inversions": 0,
        "backward": 0,
        "clock_faults": 2,
        "jitter": "0.250",
        "nominal": "10000 µs (commanded)",
    }
    observed = health_rows(snapshot(stats(5)))[0]["nominal"]
    assert observed == "10000 µs (observed)"


def test_the_commander_line_names_the_phase_and_the_detail() -> None:
    state = ControllerState(
        Phase.LIVE,
        True,
        False,
        frozenset({5}),
        {5: 10_000},
        "'tare 7 0' was refused: # err not_ready",
        None,
        0,
    )
    text = controller_text(state)
    assert text.startswith("commands: live")
    assert "refused" in text


def test_the_calibration_line_decodes_the_cal_response() -> None:
    text = calibration_text(CalibrationStatus(0, 1, 1, 0, 0, 0))
    assert text == (
        "status 0 (0 is success); accelerometer on; gyroscope on; "
        "magnetometer off; planar accelerometer off; on-table off"
    )
    assert calibration_text(None) == "no calibration response yet"


def test_the_response_line_is_shown_as_received() -> None:
    line = "# resp cmd=03 seq=9 rseq=1 r=0000000000000000000000"
    assert response_text(line) == f"last command response: {line}"
    assert response_text("") == "no command response yet"


def test_the_replay_line_shows_the_progress_and_a_hold() -> None:
    assert progress_text(0.5, False) == "replay at 50%"
    assert progress_text(0.5, True) == "replay at 50%, held"


def test_the_recording_line_names_the_file_or_the_control_message() -> None:
    assert recording_text(True, "session-1.txt", "") == "recording to session-1.txt"
    assert recording_text(False, "", "") == "not recording"
    # The control's message is shown as its own word: a refusal to start
    # while one is active, or the reason a recording failed.
    assert recording_text(False, "", "a recording is already active") == (
        "a recording is already active"
    )
    assert recording_text(False, "", "the recording failed: full disk") == (
        "the recording failed: full disk"
    )


def test_the_no_board_reason_is_stated() -> None:
    assert "replay" in NO_BOARD


def test_the_summary_counts_replies_and_rejected_lines() -> None:
    assert summary(snapshot()) == ("4 replies, 2 lines not part of the protocol")


def test_the_commanded_rate_is_rounded_to_the_nearest_hertz() -> None:
    assert commanded_hz(2_500) == "400 Hz"
    assert commanded_hz(3_000) == "333 Hz"
    assert commanded_hz(7_000) == "143 Hz"


def test_the_health_columns_name_the_jitter_unit() -> None:
    labels = {column["label"] for column in HEALTH_COLUMNS}
    # The bench's own name for the figure is p99_jitter_intervals, and the
    # label says so rather than showing a bare ratio.
    assert "p99 jitter (intervals)" in labels


def test_the_rotation_select_offers_the_three_rotation_kinds() -> None:
    options = rotation_options()
    assert set(options) == {0x05, 0x08, 0x2A}
    assert options[0x05] == "Rotation vector (0x05)"
    assert options[0x2A] == "Gyro-integrated rotation vector (0x2A)"


def test_each_rotation_source_names_what_it_is_referenced_to() -> None:
    assert "magnetic north" in reference_text(0x05)
    assert "gravity" in reference_text(0x08)
    assert "no heading reference" in reference_text(0x08)
    assert "UNCONFIRMED" in reference_text(0x2A)
    assert "LP-6" in reference_text(0x05)
    assert "LP-6" in reference_text(0x08)
    assert "LP-6" in reference_text(0x2A)


def test_the_euler_convention_is_stated_and_the_game_source_notes_heading() -> None:
    convention = convention_text(0x05)
    assert "yaw about Z" in convention
    assert "pitch about the new Y" in convention
    assert "roll about the newest X" in convention
    assert "(-180, 180]" in convention
    assert "[-90, 90]" in convention
    assert "heading" not in convention
    assert convention_text(0x08) == f"{convention} Yaw has no heading reference."


def test_the_euler_line_formats_the_selected_angles() -> None:
    orientation = orientation_of((0.24121, -0.03625, 0.03137, -0.96930))
    assert orientation is not None
    line = euler_label(0x05, orientation, True)
    assert line.startswith("yaw ")
    assert "pitch " in line
    assert "roll " in line
    assert line.count("°") == 3


def test_the_euler_line_never_prints_a_signed_zero() -> None:
    """A reading that rounds to zero is a zero, whichever side it sat on."""
    identity = orientation_of((1.0, 0.0, 0.0, 0.0))
    assert identity is not None
    assert euler_label(0x05, identity, True) == "yaw 0.00°, pitch 0.00°, roll 0.00°"
    half = math.radians(-0.004) / 2.0
    barely = orientation_of((math.cos(half), 0.0, math.sin(half), 0.0))
    assert barely is not None
    assert barely.euler.pitch_deg < 0.0
    line = euler_label(0x05, barely, True)
    assert "-0.00" not in line
    assert line == "yaw 0.00°, pitch 0.00°, roll 0.00°"


def test_the_euler_line_without_samples_or_without_a_rotation() -> None:
    no_samples = euler_label(0x08, None, False)
    assert "no samples" in no_samples
    assert "Game rotation vector (0x08)" in no_samples
    no_rotation = euler_label(0x05, None, True)
    assert "no orientation" in no_rotation


def test_the_missing_rotation_note_names_the_sensor_to_enable() -> None:
    note = missing_rotation_text(0x2A)
    assert "no samples" in note
    assert sensor_name(0x2A) in note


def test_the_motion_notes_state_the_window_or_the_absence() -> None:
    assert "no samples" in no_motion_text(0x03)
    assert "Magnetometer (0x03)" in no_motion_text(0x03)
    window = motion_window_text(0x02)
    assert "rad/s" in window
    assert "seconds" in window
    assert "5" in window


def test_the_motion_options_carry_one_series_per_axis() -> None:
    points = (((0.0, 1.0), (1.0, 2.0)),) * 3
    motion = Motion(1.0, points)
    options = motion_options(0x01, motion)
    series = cast("list[dict[str, object]]", options["series"])
    assert [entry["name"] for entry in series] == ["x", "y", "z"]
    assert series[0]["data"] == [[0.0, 1.0], [1.0, 2.0]]
    axis = cast("dict[str, object]", options["xAxis"])
    assert axis["min"] == pytest.approx(1.0 - 5.0)
    assert axis["max"] == pytest.approx(1.0)


def test_the_motion_options_of_no_samples_are_empty_series() -> None:
    options = motion_options(0x03, None)
    series = cast("list[dict[str, object]]", options["series"])
    assert [entry["data"] for entry in series] == [[], [], []]
