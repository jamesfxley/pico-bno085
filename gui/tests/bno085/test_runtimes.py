# SPDX-License-Identifier: MIT
import threading
import time
from collections import deque
from pathlib import Path

import pytest

from pico_gui.recorder import Recorder, RecorderSlot
from pico_gui.session import State
from pico_gui.transport import TransportError
from pico_gui_bno085.__main__ import (
    BoardRuntime,
    RecorderControl,
    ReplayRuntime,
    close_recordings,
)
from pico_gui_bno085.commands import cfg
from pico_gui_bno085.controller import Phase
from tests.core.test_recorder import FailingStream

DATA = Path(__file__).resolve().parent.parent / "data" / "session.txt"
WAIT_SECONDS = 5.0


def replayed(path: Path) -> ReplayRuntime:
    """Return a replay over `path` that reads as fast as the file allows."""
    return ReplayRuntime(path, lines_per_second=None, recorder=None)


def test_a_replay_reads_the_whole_recording_into_its_model() -> None:
    runtime = replayed(DATA)
    runtime.start()
    runtime.wait(WAIT_SECONDS)
    assert runtime.status().state is State.ENDED
    assert runtime.progress() == 1.0
    assert not runtime.paused()
    assert runtime.snapshot().sensors
    runtime.shutdown()


def test_a_restart_reads_the_recording_again_into_a_fresh_model() -> None:
    runtime = replayed(DATA)
    runtime.start()
    runtime.wait(WAIT_SECONDS)
    first = runtime.snapshot()
    runtime.restart()
    runtime.wait(WAIT_SECONDS)
    second = runtime.snapshot()
    assert second.replies == first.replies
    assert [stats.sensor for stats in second.sensors] == [
        stats.sensor for stats in first.sensors
    ]
    assert runtime.progress() == 1.0
    runtime.shutdown()


def test_a_restart_counts_each_line_once_in_a_fresh_model() -> None:
    """A model kept from the first pass would count every line twice."""
    runtime = replayed(DATA)
    runtime.start()
    runtime.wait(WAIT_SECONDS)
    first = runtime.snapshot()
    runtime.restart()
    runtime.wait(WAIT_SECONDS)
    second = runtime.snapshot()
    assert first.sensors
    assert second.replies == first.replies
    assert [stats.figures.count for stats in second.sensors] == [
        stats.figures.count for stats in first.sensors
    ]
    runtime.shutdown()


def test_a_replay_restart_is_not_a_clock_fault() -> None:
    """The second pass starts at the recording's first time, after the last."""
    runtime = replayed(DATA)
    runtime.start()
    runtime.wait(WAIT_SECONDS)
    runtime.restart()
    runtime.wait(WAIT_SECONDS)
    assert [s.figures.clock_faults for s in runtime.snapshot().sensors] == [
        0 for _ in runtime.snapshot().sensors
    ]
    runtime.shutdown()


def test_the_replay_surface_holds_and_releases_the_pass() -> None:
    runtime = replayed(DATA)
    surface = runtime.surface()
    runtime.start()
    runtime.wait(WAIT_SECONDS)
    assert not surface.paused()
    surface.pause()
    assert surface.paused()
    surface.resume()
    assert not surface.paused()
    surface.restart()
    runtime.wait(WAIT_SECONDS)
    assert runtime.progress() == 1.0
    runtime.shutdown()


def test_the_runtime_reports_no_session_until_it_starts() -> None:
    runtime = replayed(DATA)
    assert runtime.status().state is State.ENDED
    assert runtime.progress() == 1.0


def test_the_recorder_control_generates_names_and_records_into_them(
    tmp_path: Path,
) -> None:
    control = RecorderControl(tmp_path)
    control.start()
    recorded = tmp_path / control.surface().name()
    assert recorded.name.startswith("session-")
    assert control.surface().active()
    # The slot the session writes through is the control's own.
    control.slot.write("E,5,0,100,3")
    control.stop()
    assert not control.surface().active()
    assert recorded.read_bytes() == b"E,5,0,100,3\n"


def test_the_recorder_control_avoids_a_name_already_taken(tmp_path: Path) -> None:
    control = RecorderControl(tmp_path)
    control.start()
    control.stop()
    first = control.surface().name()
    # A second recording in the same second takes a numbered suffix.
    control.start()
    second = control.surface().name()
    control.stop()
    assert second == first.replace(".txt", "-1.txt")


def test_a_recording_that_cannot_be_opened_is_reported_by_the_caller(
    tmp_path: Path,
) -> None:
    control = RecorderControl(tmp_path / "absent")
    with pytest.raises(TransportError, match="cannot write the recording"):
        control.start()
    assert not control.surface().active()


def test_a_write_failure_reaches_the_surface_without_ending_the_slot() -> None:
    failures: list[str] = []
    slot = RecorderSlot(on_failure=failures.append)
    slot.attach(Recorder(FailingStream(fail_write=True)))
    slot.write("# ready")
    assert "no space left on device" in failures[0]


class ScriptedBoardTransport:
    """A queue-fed `recv` and a recording `send` for the board runtime."""

    def __init__(self, chunks: list[bytes]) -> None:
        self._chunks = deque(chunks)
        self.sent: list[bytes] = []
        self.sent_event = threading.Event()
        self.closed = threading.Event()

    def recv(self, _size: int, /) -> bytes:
        if self.closed.is_set():
            return b""
        if self._chunks:
            return self._chunks.popleft()
        raise TimeoutError

    def send(self, data: bytes, /) -> None:
        self.sent.append(data)
        self.sent_event.set()

    def feed(self, chunk: bytes) -> None:
        """Queue the next bytes the board would send."""
        self._chunks.append(chunk)

    def close(self) -> None:
        self.closed.set()


def await_sends(transport: ScriptedBoardTransport, count: int) -> None:
    """Wait until the transport has sent `count` commands, bounded."""
    deadline = time.monotonic() + WAIT_SECONDS
    while len(transport.sent) < count:
        remaining = deadline - time.monotonic()
        assert remaining > 0, f"only {len(transport.sent)} sends"
        transport.sent_event.wait(remaining)
        transport.sent_event.clear()


def sent_lines(transport: ScriptedBoardTransport) -> list[str]:
    return [line.decode("ascii").rstrip("\n") for line in transport.sent]


def test_the_board_runtime_drives_the_bench_from_a_scripted_stream() -> None:
    transport = ScriptedBoardTransport(
        [b"# ready\n", b"# bus spi\n", b"# payload 1\n"],
    )
    runtime = BoardRuntime(transport, "spi", None)
    # The connect sequence waits for each reply before the next command.
    await_sends(transport, 2)
    assert sent_lines(transport) == ["bus spi", "payload 1"]
    assert runtime.surface().state().phase is Phase.LIVE

    runtime.surface().submit(cfg(0x05, 10_000))
    await_sends(transport, 3)
    assert sent_lines(transport)[-1] == "cfg 05 10000"
    transport.feed(b"# ok\n")
    await_sends(transport, 4)
    assert sent_lines(transport)[-1] == "run 1000"
    transport.feed(
        b"# run 1000\n"
        b"Q,5,0,3777014117,0,0.24121,-0.03625,0.03137,-0.96930,3.14160\n"
        b"# done events=1\n",
    )
    # The done line re-arms the run, so the second run command follows.
    await_sends(transport, 5)
    assert sent_lines(transport)[-1] == "run 1000"
    state = runtime.surface().state()
    assert state.enabled == {0x05}
    # The re-armed run is in flight, awaiting its own reply.
    assert state.in_flight == "run 1000"
    assert not state.streaming
    # The telemetry line reached the model, and the session's own cfg is
    # the nominal interval of its figures.
    snapshot = runtime.snapshot()
    assert [stats.sensor for stats in snapshot.sensors] == [5]
    assert snapshot.sensors[0].figures.nominal_is_commanded
    assert snapshot.sensors[0].figures.nominal_interval_us == 10_000

    runtime.shutdown()
    assert transport.closed.is_set()


def test_the_board_runtime_shutdown_ends_the_session() -> None:
    transport = ScriptedBoardTransport([])
    runtime = BoardRuntime(transport, "i2c", None)
    await_sends(transport, 1)
    runtime.shutdown()
    assert runtime.status().state is State.ENDED


def test_a_recording_keeps_writing_across_replay_restarts(tmp_path: Path) -> None:
    control = RecorderControl(tmp_path)
    control.start()
    runtime = ReplayRuntime(DATA, lines_per_second=None, recorder=control.slot)
    runtime.start()
    runtime.wait(WAIT_SECONDS)
    runtime.restart()
    runtime.wait(WAIT_SECONDS)
    # The pass ended and the pass after it wrote into the same recording.
    assert control.surface().active()
    control.stop()
    runtime.shutdown()
    recorded = (
        (tmp_path / control.surface().name())
        .read_text(
            encoding="ascii",
        )
        .splitlines()
    )
    once = DATA.read_text(encoding="ascii").splitlines()
    assert recorded == once + once


def test_the_record_button_refuses_while_a_recording_is_active(
    tmp_path: Path,
) -> None:
    control = RecorderControl(tmp_path)
    control.start()
    first = control.surface().name()
    control.start()
    # The refusal is the panel's message; no second file is opened.
    assert control.surface().failure() == "a recording is already active; stop it first"
    control.stop()
    assert [path.name for path in tmp_path.iterdir()] == [first]


def test_a_failed_close_at_shutdown_is_said_and_swallowed(
    capsys: pytest.CaptureFixture[str],
) -> None:
    slot = RecorderSlot(Recorder(FailingStream(fail_close=True)))
    close_recordings(slot)
    error = capsys.readouterr().err
    assert "input/output error" in error
    assert "pico_gui_bno085" in error


def test_close_recordings_leaves_an_empty_slot_alone() -> None:
    slot = RecorderSlot()
    close_recordings(slot, None)
    assert not slot.attached


def test_the_refusal_message_is_cleared_by_stop(tmp_path: Path) -> None:
    control = RecorderControl(tmp_path)
    control.start()
    control.start()
    assert control.surface().failure() == "a recording is already active; stop it first"
    # Stopping the recording clears the refusal, so the state line does not
    # keep showing a message about a recording that no longer runs.
    control.stop()
    assert control.surface().failure() == ""
    assert control.surface().active() is False
