# SPDX-License-Identifier: MIT
import os
import socket
import subprocess
import sys
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
        ["--serial", "S", "--port", "9100", "--device-port", "PORT-A"],
    )
    assert args.port == 9100
    assert args.device_port == "PORT-A"


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


@pytest.mark.parametrize("rate", ["0", "-5", "nan", "inf", "fast"])
def test_a_replay_rate_that_is_not_positive_is_refused(rate: str) -> None:
    with pytest.raises(SystemExit) as caught:
        parser().parse_args(["--replay", "x.txt", "--replay-rate", rate])
    assert caught.value.code == 2


def test_a_positive_replay_rate_is_accepted() -> None:
    args = parser().parse_args(["--replay", "x.txt", "--replay-rate", "0.5"])
    assert args.replay_rate == 0.5


def test_a_port_in_use_exits_non_zero_with_a_message_and_no_steps(
    tmp_path: Path,
) -> None:
    replay = tmp_path / "empty.txt"
    replay.write_bytes(b"")
    with socket.socket() as taken:
        taken.bind(("127.0.0.1", 0))
        taken.listen()
        port = taken.getsockname()[1]
        done = subprocess.run(
            [
                sys.executable,
                "-m",
                "pico_gui_bno085",
                "--replay",
                str(replay),
                "--port",
                str(port),
            ],
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
            # NiceGUI switches to its test mode when it sees pytest's variable.
            env={k: v for k, v in os.environ.items() if k != "PYTEST_CURRENT_TEST"},
        )
    assert done.returncode != 0
    assert f"cannot serve on 127.0.0.1:{port}" in done.stderr
    assert "Forward a Port" not in done.stdout
    assert "listening" not in done.stdout
