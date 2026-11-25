# SPDX-License-Identifier: MIT
from pico_gui_bno085.model import SensorStats, Snapshot
from pico_gui_bno085.panel import SENSOR_NAMES, row_of, sensor_name, summary
from pico_gui_bno085.protocol import Kind, Telemetry


def test_a_known_sensor_has_a_name_and_its_id() -> None:
    assert sensor_name(0x2A) == "Gyro-integrated rotation vector (0x2A)"


def test_an_unknown_sensor_is_still_shown() -> None:
    assert sensor_name(0x7E) == "Sensor (0x7E)"


def test_every_sensor_of_lp6_has_a_name() -> None:
    lp6 = {0x05, 0x09, 0x28, 0x08, 0x29, 0x01, 0x02, 0x03, 0x04, 0x06, 0x07, 0x0F, 0x2A}
    assert lp6 == set(SENSOR_NAMES)


def test_a_row_shows_rate_count_and_missing() -> None:
    latest = Telemetry(Kind.Q, 5, 0, 0, 3, (0.0,) * 5)
    row = row_of(SensorStats(5, latest, 12, 99.96, 3, 0))
    assert row == {
        "sensor": "Rotation vector (0x05)",
        "rate": "100.0",
        "count": 12,
        "missing": 3,
    }


def test_the_summary_counts_replies_and_rejected_lines() -> None:
    assert summary(Snapshot((), 4, 2, None)) == (
        "4 replies, 2 lines not part of the protocol"
    )
