# SPDX-License-Identifier: MIT
import os
import select
import signal
import socket
import subprocess
import sys
import time
from collections.abc import Iterator
from pathlib import Path
from typing import ClassVar, Self

import pytest

from pico_gui import launch
from pico_gui_bno085 import __main__ as launcher
from pico_gui_bno085.__main__ import main, parser

DATA = Path(__file__).resolve().parent.parent / "data"


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


def test_a_bus_with_a_replay_is_refused(
    capsys: pytest.CaptureFixture[str],
) -> None:
    with pytest.raises(SystemExit) as caught:
        main(["--replay", "x.txt", "--bus", "spi"])
    assert caught.value.code == 2
    assert "--bus" in capsys.readouterr().err


def test_a_serial_without_a_bus_is_refused(
    capsys: pytest.CaptureFixture[str],
) -> None:
    with pytest.raises(SystemExit) as caught:
        main(["--serial", "S"])
    assert caught.value.code == 2
    assert "--bus is required" in capsys.readouterr().err


def test_record_and_record_dir_are_refused_together(
    capsys: pytest.CaptureFixture[str],
) -> None:
    with pytest.raises(SystemExit) as caught:
        main(["--replay", "x.txt", "--record", "a.txt", "--record-dir", "dir"])
    assert caught.value.code == 2
    assert "--record" in capsys.readouterr().err


def test_the_server_port_and_the_device_port_are_separate_options() -> None:
    args = parser().parse_args(
        ["--serial", "S", "--bus", "spi", "--port", "9100", "--device-port", "PORT-A"],
    )
    assert args.port == 9100
    assert args.device_port == "PORT-A"
    assert args.bus == "spi"


def test_the_record_dir_option_takes_a_directory() -> None:
    args = parser().parse_args(["--replay", "x.txt", "--record-dir", "records"])
    assert args.record_dir == Path("records")


def test_a_missing_recording_exits_with_a_message(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    status = main(["--replay", str(tmp_path / "absent.txt")])
    assert status == 2
    error = capsys.readouterr().err
    assert "cannot open the recording" in error
    assert "absent.txt" in error


def test_a_missing_recording_closes_the_record_file(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    record = tmp_path / "record.txt"
    status = main(["--replay", str(tmp_path / "absent.txt"), "--record", str(record)])
    assert status == 2
    assert "cannot open the recording" in capsys.readouterr().err


def test_a_missing_device_proxy_exits_with_a_message(
    capsys: pytest.CaptureFixture[str],
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    monkeypatch.chdir(tmp_path)
    status = main(["--serial", "S", "--bus", "spi"])
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
    """Return a port the caller can bind soon after.

    The probe closes before the caller binds, so another process can take
    the port in the gap. A second bind of the same port catches one that
    is taken, and the probe tries once more before giving up.
    """
    for attempt in (0, 1):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = int(probe.getsockname()[1])
        with socket.socket() as check:
            try:
                check.bind(("127.0.0.1", port))
            except OSError:
                if attempt == 1:
                    raise
            else:
                return port
    raise AssertionError("neither probe found a port the check could bind")


class FakeSocket:
    """A socket whose chosen port the test scripts and may refuse."""

    ports: ClassVar[Iterator[int]] = iter(())
    taken: ClassVar[set[int]] = set()

    def __enter__(self) -> "Self":
        return self

    def __exit__(self, *_: object) -> None:
        pass

    def bind(self, address: tuple[str, int]) -> None:
        if address[1] in self.taken:
            raise OSError(f"the port {address[1]} is taken")

    def getsockname(self) -> tuple[str, int]:
        return ("127.0.0.1", next(self.ports))


def scripted_free_port(monkeypatch: pytest.MonkeyPatch, ports: list[int]) -> None:
    """Make `free_port` choose from `ports` and take every one but the last."""
    FakeSocket.ports = iter(ports)
    FakeSocket.taken = set(ports[:-1])
    monkeypatch.setattr(socket, "socket", FakeSocket)


def test_free_port_retries_once_on_a_taken_port(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    scripted_free_port(monkeypatch, [11111, 22222])
    assert free_port() == 22222


def test_free_port_gives_up_when_the_retry_is_taken_too(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    scripted_free_port(monkeypatch, [11111, 11111])
    with pytest.raises(OSError, match="taken"):
        free_port()


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
            assert "notification will tell you the localhost port" in steps
            assert "for example http://localhost:4123" in steps
            process.send_signal(signal.SIGINT)
            rest_bytes, _ = process.communicate(timeout=STARTUP_SECONDS)
        finally:
            if process.poll() is None:
                process.kill()
    assert "Server stopped." in rest_bytes.decode("utf-8", errors="replace")
    assert process.returncode == 0


def test_a_record_file_that_exists_is_refused_before_anything_opens(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    existing = tmp_path / "existing.txt"
    existing.write_bytes(b"# ready\n")
    status = main(["--replay", str(tmp_path / "absent.txt"), "--record", str(existing)])
    assert status == 2
    error = capsys.readouterr().err
    assert "exists" in error
    assert str(existing) in error
    assert "cannot open the recording" not in error
    assert existing.read_bytes() == b"# ready\n"


def test_the_replayed_file_is_not_recorded_over(
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    recording = tmp_path / "session.txt"
    content = (DATA / "session.txt").read_bytes()
    recording.write_bytes(content)
    status = main(["--replay", str(recording), "--record", str(recording)])
    assert status == 2
    assert "exists" in capsys.readouterr().err
    assert recording.read_bytes() == content


def test_a_fresh_record_file_is_created(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(
        launch,
        "run",
        lambda _title, _server, _build, shutdown: shutdown(),
    )
    record = tmp_path / "fresh.txt"
    status = main(["--replay", str(DATA / "session.txt"), "--record", str(record)])
    assert status == 0
    assert record.exists()


def test_an_existing_record_file_is_refused_before_the_board_is_opened(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    def forbidden(*_args: object, **_kwargs: object) -> None:
        raise AssertionError("the board was opened")

    monkeypatch.setattr(launcher, "open_device", forbidden)
    existing = tmp_path / "existing.txt"
    existing.write_bytes(b"# ready\n")
    status = main(["--serial", "S", "--bus", "spi", "--record", str(existing)])
    assert status == 2
    assert "exists" in capsys.readouterr().err
    assert existing.read_bytes() == b"# ready\n"
