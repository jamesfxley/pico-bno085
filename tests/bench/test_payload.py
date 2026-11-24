# SPDX-License-Identifier: MIT
"""The harness counts E lines, so it turns payload lines off first (LP-5)."""

import importlib.util
import sys
from pathlib import Path

import pytest

BENCH = Path(__file__).resolve().parents[2] / "examples/bench/bench.py"
SPEC = importlib.util.spec_from_file_location("bno_bench_payload", BENCH)
bench = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bench)


class RecordingDevice:
    def __init__(self, reply):
        self.reply = reply
        self.log = []
        self.bus = None
        self.wire = None

    def send(self, line):
        self.log.append(line)

    def wait_for(self, prefix, _timeout):
        return prefix if self.reply else None


def test_payload_is_switched_off_and_acknowledged():
    dev = RecordingDevice(reply=True)
    assert bench.disable_payload(dev) is True
    assert dev.log == ["payload 0"]


def test_a_silent_firmware_is_reported():
    assert bench.disable_payload(RecordingDevice(reply=False)) is False


def test_a_session_disables_payload_before_it_selects_the_bus(monkeypatch):
    dev = RecordingDevice(reply=True)
    order = []
    monkeypatch.setattr(bench, "Device", lambda *_args: dev)
    monkeypatch.setattr(
        bench, "select_bus",
        lambda device, bus: order.append(("bus", bus, tuple(device.log))) or True,
    )
    monkeypatch.setitem(
        bench.TESTS, "b-boot", lambda _dev, _cfg: {"test": "b-boot", "passed": True},
    )
    argv = ["bench.py", "--port", "unused", "--bus", "spi", "--tests", "b-boot"]
    monkeypatch.setattr(sys, "argv", argv)
    with pytest.raises(SystemExit) as exit_info:
        bench.main()
    assert exit_info.value.code == 0
    assert order == [("bus", "spi", ("payload 0",))]
