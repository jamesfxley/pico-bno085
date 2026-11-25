# SPDX-License-Identifier: MIT
from pathlib import Path

import pytest

from pico_gui_bno085.__main__ import main, parser


def test_a_source_is_required() -> None:
    with pytest.raises(SystemExit) as caught:
        parser().parse_args([])
    assert caught.value.code == 2


def test_serial_and_replay_exclude_each_other() -> None:
    with pytest.raises(SystemExit) as caught:
        parser().parse_args(["--serial", "S", "--replay", "x.txt"])
    assert caught.value.code == 2


def test_the_server_port_and_the_device_port_are_separate_options() -> None:
    args = parser().parse_args(
        ["--serial", "S", "--port", "9100", "--device-port", "COM7"],
    )
    assert args.port == 9100
    assert args.device_port == "COM7"


def test_a_missing_recording_exits_with_a_message(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    status = main(["--replay", str(tmp_path / "absent.txt")])
    assert status == 2
    error = capsys.readouterr().err
    assert "cannot open the recording" in error
    assert "absent.txt" in error


def test_a_missing_device_proxy_exits_with_a_message(
    capsys: pytest.CaptureFixture[str],
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    monkeypatch.chdir(tmp_path)
    status = main(["--serial", "S"])
    assert status == 2
    assert "proxy" in capsys.readouterr().err
