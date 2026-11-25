# SPDX-License-Identifier: MIT
import os
import select
import signal
import socket
import subprocess
import sys
import time
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


def test_a_device_port_with_a_replay_is_refused(
    capsys: pytest.CaptureFixture[str],
) -> None:
    with pytest.raises(SystemExit) as caught:
        main(["--replay", "x.txt", "--device-port", "PORT-A"])
    assert caught.value.code == 2
    assert "--device-port" in capsys.readouterr().err


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


SESSION = Path(__file__).resolve().parent.parent / "data" / "session.txt"
STARTUP_SECONDS = 60.0
# NiceGUI switches to its test mode when it sees pytest's variable.
CLEAN_ENV = {k: v for k, v in os.environ.items() if k != "PYTEST_CURRENT_TEST"}
COUNTED_SHUTDOWN = """
import sys
from pico_gui import launch

def build() -> None:
    pass

def shutdown() -> None:
    print("on_shutdown called", flush=True)

launch.run("t", launch.Server("127.0.0.1", int(sys.argv[1])), build, shutdown)
"""


def free_port() -> int:
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        return int(probe.getsockname()[1])


def test_on_shutdown_runs_once_when_the_port_is_in_use() -> None:
    with socket.socket() as taken:
        taken.bind(("127.0.0.1", 0))
        taken.listen()
        port = taken.getsockname()[1]
        done = subprocess.run(
            [sys.executable, "-c", COUNTED_SHUTDOWN, str(port)],
            capture_output=True,
            text=True,
            timeout=60,
            check=False,
            env=CLEAN_ENV,
        )
    assert done.returncode != 0
    assert done.stdout.count("on_shutdown called") == 1


def read_until(process: subprocess.Popen[bytes], marker: str) -> str:
    """Return the standard output of `process` up to the text `marker`.

    Reads the pipe's file descriptor, so that `select` sees what is pending.
    """
    assert process.stdout is not None
    descriptor = process.stdout.fileno()
    deadline = time.monotonic() + STARTUP_SECONDS
    seen = ""
    while marker not in seen:
        remaining = deadline - time.monotonic()
        assert remaining > 0, f"no {marker!r} in {seen!r}"
        ready, _, _ = select.select([descriptor], [], [], remaining)
        assert ready, f"no {marker!r} in {seen!r}"
        chunk = os.read(descriptor, 4096)
        assert chunk, f"the process ended before {marker!r}: {seen!r}"
        seen += chunk.decode("utf-8", errors="replace")
    return seen


def test_a_started_server_prints_the_steps_and_stops_on_interrupt() -> None:
    port = free_port()
    with subprocess.Popen(
        [
            sys.executable,
            "-u",
            "-m",
            "pico_gui_bno085",
            "--replay",
            str(SESSION),
            "--port",
            str(port),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        env=CLEAN_ENV,
    ) as process:
        try:
            steps = read_until(process, "Press Ctrl+C")
            assert f"listening on port {port}" in steps
            assert f"http://localhost:{port}" in steps
            process.send_signal(signal.SIGINT)
            rest_bytes, _ = process.communicate(timeout=STARTUP_SECONDS)
        finally:
            if process.poll() is None:
                process.kill()
    assert "Server stopped." in rest_bytes.decode("utf-8", errors="replace")
    assert process.returncode == 0
