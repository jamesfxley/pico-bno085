# SPDX-License-Identifier: MIT
"""OR-1 / PR-3: wire evidence diagnoses timing without changing verdicts."""

import importlib.util
from pathlib import Path
from types import SimpleNamespace

import pytest

SPEC = importlib.util.spec_from_file_location(
    "bno_bench", Path(__file__).resolve().parents[2] / "examples/bench/bench.py",
)
bench = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bench)


def test_base_rebase_and_two_sensor_delays():
    # 1000-3625 §§7.2.1-7.2.2, §§6.5.13/18: base 10, rebase -3;
    # gyro delay 2, RV delay 0x102. 1000-3927 §1.3.5.3 adds delay.
    frame = bytes.fromhex(
        "26 00 03 00 fb 0a 00 00 00 "
        "02 07 03 02 00 00 00 00 00 00 "
        "fa fd ff ff ff "
        "05 08 07 02 00 00 00 00 00 00 00 00 00 00",
    )
    events, grants = bench.decode_wire_transfer(1_000_000, frame)
    assert grants == []
    assert [(e["sensor"], e["seq"], e["event_us"]) for e in events] == [
        (2, 7, 999_200),
        (5, 8, 1_024_500),
    ]
    assert events[1]["delay_ticks"] == 258
    assert events[1]["rebases"] == -3


def test_signed_base_and_positive_rebase():
    # A signed base of -10 moves forward; rebase +3, delay +2.
    cargo = bytes.fromhex("fb f6 ff ff ff fa 03 00 00 00 02 09 00 02 00 00 00 00 00 00")
    assert bench.decode_wire_input(100_000, cargo)[0]["event_us"] == 101_500


def test_get_feature_response_fields():
    # 1000-3625 §6.5.5: FC, RV, flags, sensitivity, interval, batch, config.
    frame = bytes.fromhex(
        "15 00 02 07 fc 05 04 02 00 20 4e 00 00 00 00 00 00 78 56 34 12",
    )
    events, grants = bench.decode_wire_transfer(123, frame)
    assert events == []
    assert grants == [
        {
            "sensor": 5,
            "flags": 4,
            "change_sensitivity": 2,
            "report_interval_us": 20000,
            "batch_interval_us": 0,
            "sensor_specific": 0x12345678,
        },
    ]


@pytest.mark.parametrize(
    ("frame", "message"),
    [
        (b"", "short transfer header"),
        (bytes.fromhex("05 80 03 00 00"), "fragmented or length-mismatched"),
        (bytes.fromhex("06 00 03 00 fb"), "fragmented or length-mismatched"),
        (bytes.fromhex("05 00 03 00 ff"), "unknown or truncated input"),
        (bytes.fromhex("09 00 03 00 fb ff ff ff 7f"), "overflowed base"),
        (bytes.fromhex("05 00 02 00 fc"), "truncated or combined feature"),
    ],
)
def test_unusable_wire_is_explicit(frame, message):
    with pytest.raises(ValueError, match=message):
        bench.decode_wire_transfer(1000, frame)


def test_capture_grants_and_pairs_by_sensor_sequence():
    capture = bench.WireCapture()
    capture.feed(
        "# wire edge_us=0 n=21: "
        "15 00 02 07 fc 05 04 02 00 20 4e 00 00 00 00 00 00 78 56 34 12",
    )
    capture.feed("# run 5000")
    capture.feed(
        "# wire edge_us=10000 n=19: "
        "13 00 03 00 fb 0a 00 00 00 02 ff 00 02 00 00 00 00 00 00",
    )
    capture.feed("# done events=1")
    capture.feed(
        "# wire edge_us=20000 n=19: "
        "13 00 03 00 fb 0a 00 00 00 02 00 00 02 00 00 00 00 00 00",
    )
    result = capture.compare(
        [
            {"sensor": 2, "seq": 255, "event_us": 9201},
            {"sensor": 5, "seq": 255, "event_us": 9200},
        ],
    )
    assert result["summary"]["matched"] == 1
    assert result["summary"]["disagreements"] == 1
    assert result["pairs"][0]["driver_minus_wire_us"] == 1
    assert result["summary"]["unmatched_driver"] == 1
    assert result["summary"]["unmatched_wire"] == 0
    assert result["summary"]["granted_intervals"][0]["report_interval_us"] == 20000


def test_trace_errors_are_retained():
    capture = bench.WireCapture()
    capture.feed("# wire edge_us=0 err=3")
    capture.feed("# wire edge_us=0 n=8: 00")
    assert len(capture.errors) == 2


class FakeDevice:
    def __init__(self, offset, gyro_period_us=20000, rv_period_us=20000):
        self.wire = None
        self.sent = []
        self.offset = offset
        self.periods = {2: gyro_period_us, 5: rv_period_us}

    def send(self, command):
        self.sent.append(command)

    def wait_for(self, prefix, _timeout):
        if prefix == "# info":
            return "# info tick_us=100"
        if prefix.startswith("# trace"):
            return prefix
        if prefix == "# stats":
            return "# stats level_fallbacks=0"
        return "# ok"

    def collect_run(self, _duration):
        return [
            {"sensor": sensor, "seq": i,
             "event_us": 100000 + self.periods[sensor] * i + offset}
            for i in range(250)
            for sensor, offset in [(2, 0), (5, self.offset)]
        ], {"level_fallbacks": 0}


@pytest.mark.parametrize("offset", [0, 10000])
def test_phase_is_recorded_not_gated(tmp_path, offset):
    for wire in [False, True]:
        dev = FakeDevice(offset)
        cfg = SimpleNamespace(rate_hz=50, duration=5, wire=wire, output_dir=tmp_path)
        result = bench.case_rvphase(dev, cfg)
        assert result["passed"] is True
        assert result["worst_offset_us"] == offset
        assert "off 2a" in dev.sent
        if wire:
            assert result["wire"]["unmatched_driver"] == 500
            assert Path(result["wire"]["sidecar"]).is_file()


def test_wire_phase_does_not_use_driver_times():
    capture = bench.WireCapture()
    capture.feed("# run 5000")
    capture.feed(
        "# wire edge_us=100000 n=33: "
        "21 00 03 00 fb 0a 00 00 00 "
        "02 07 00 02 00 00 00 00 00 00 "
        "05 08 00 02 00 00 00 00 00 00 00 00 00 00",
    )
    result = capture.compare([
        {"sensor": 2, "seq": 7, "event_us": 99200},
        {"sensor": 5, "seq": 8, "event_us": 109200},
    ])
    assert result["summary"]["matched"] == 2
    assert result["summary"]["disagreements"] == 1
    assert result["summary"]["wire_worst_offset_us"] == 0
    assert result["pairs"][1]["driver_minus_wire_us"] == 10000


@pytest.mark.parametrize(("rate_hz", "gyro_period_us", "rv_period_us"), [
    (50, 20000, 10000),  # both inside the granted band, but not one rate
    (500, 2000, 2000),  # one rate, but the interval is under ten bounds
])
def test_an_invalid_phase_measurement_fails(tmp_path, rate_hz, gyro_period_us,
                                             rv_period_us):
    dev = FakeDevice(0, gyro_period_us, rv_period_us)
    cfg = SimpleNamespace(rate_hz=rate_hz, duration=5, wire=False, output_dir=tmp_path)
    assert bench.case_rvphase(dev, cfg)["passed"] is False
