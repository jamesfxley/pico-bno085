# SPDX-License-Identifier: MIT
import io
import threading
from pathlib import Path

import pytest

from pico_gui.recorder import Recorder
from pico_gui.replay import ReplayTransport
from pico_gui.session import Session, State, Status
from pico_gui.transport import TransportError
from tests.core.test_recorder import FailingStream

WAIT_SECONDS = 5.0


class ScriptedTransport:
    """Chunks, then an end of stream or a failure; or a quiet stream."""

    def __init__(
        self,
        chunks: list[bytes],
        *,
        then: BaseException | None = None,
        quiet: bool = False,
    ) -> None:
        self._chunks = list(chunks)
        self._then = then
        self._quiet = quiet
        self._closed = threading.Event()
        self.close_count = 0

    def recv(self, _size: int, /) -> bytes:
        if self._chunks:
            return self._chunks.pop(0)
        if self._then is not None:
            raise self._then
        if self._quiet:
            if self._closed.wait(0.01):
                raise TransportError("closed")
            raise TimeoutError
        return b""

    def send(self, data: bytes, /) -> None:
        raise AssertionError(data)

    def close(self) -> None:
        self.close_count += 1
        self._closed.set()


class Collector:
    def __init__(self) -> None:
        self.lines: list[tuple[str, float]] = []

    def __call__(self, line: str, now: float) -> None:
        self.lines.append((line, now))


def test_lines_arrive_with_the_clock_reading_of_their_chunk() -> None:
    times = iter([1.0, 2.0])
    collector = Collector()
    transport = ScriptedTransport([b"a\nb\nc", b"d\n"])
    session = Session(transport, collector, clock=lambda: next(times))
    session.start()
    session.wait(WAIT_SECONDS)
    assert collector.lines == [("a", 1.0), ("b", 1.0), ("cd", 2.0)]
    assert session.status == Status(State.ENDED, "", 3, 0)


def test_an_unterminated_last_line_is_delivered_at_the_end() -> None:
    collector = Collector()
    session = Session(ScriptedTransport([b"a\nb"]), collector, clock=lambda: 0.0)
    session.start()
    session.wait(WAIT_SECONDS)
    assert [line for line, _ in collector.lines] == ["a", "b"]


def test_a_transport_failure_is_the_status_and_not_an_exception() -> None:
    transport = ScriptedTransport([b"a\n"], then=TransportError("stream lost"))
    session = Session(transport, Collector(), clock=lambda: 0.0)
    session.start()
    session.wait(WAIT_SECONDS)
    assert session.status.state is State.FAILED
    assert session.status.detail == "stream lost"
    assert session.status.lines == 1


def test_overlong_lines_are_counted_in_the_status() -> None:
    session = Session(
        ScriptedTransport([b"abcdefgh\nok\n"]),
        Collector(),
        clock=lambda: 0.0,
        line_limit=4,
    )
    session.start()
    session.wait(WAIT_SECONDS)
    assert session.status.dropped == 1
    assert session.status.lines == 1


def test_stop_ends_a_quiet_session_and_closes_the_transport() -> None:
    transport = ScriptedTransport([], quiet=True)
    session = Session(transport, Collector(), clock=lambda: 0.0)
    session.start()
    session.stop()
    assert session.status.state is State.ENDED
    assert transport.close_count == 1


def test_a_defect_in_the_callback_propagates_and_marks_the_session_failed() -> None:
    def broken(line: str, _now: float) -> None:
        raise KeyError(line)

    session = Session(ScriptedTransport([b"a\n"]), broken, clock=lambda: 0.0)
    with pytest.raises(KeyError):
        session.run()
    assert session.status.state is State.FAILED
    assert "unexpectedly" in session.status.detail


def test_a_session_starts_once() -> None:
    session = Session(ScriptedTransport([]), Collector(), clock=lambda: 0.0)
    session.start()
    session.wait(WAIT_SECONDS)
    with pytest.raises(RuntimeError, match="started"):
        session.start()


def test_a_replay_is_recorded_line_for_line(tmp_path: Path) -> None:
    data = b"# ready\nQ,5,0,1,3,0.00000,0.00000,0.00000,1.00000,0.50000\n"
    path = tmp_path / "copy.txt"
    session = Session(
        ReplayTransport(io.BytesIO(data)),
        Collector(),
        clock=lambda: 0.0,
        recorder=Recorder.open(path),
    )
    session.start()
    session.wait(WAIT_SECONDS)
    assert path.read_bytes() == data


def test_a_failed_recording_write_is_the_status() -> None:
    session = Session(
        ScriptedTransport([b"a\n"]),
        Collector(),
        clock=lambda: 0.0,
        recorder=Recorder(FailingStream(fail_write=True)),
    )
    session.start()
    session.wait(WAIT_SECONDS)
    assert session.status.state is State.FAILED
    assert "no space left on device" in session.status.detail


def test_a_failed_recording_close_is_the_status() -> None:
    session = Session(
        ScriptedTransport([b"a\n"]),
        Collector(),
        clock=lambda: 0.0,
        recorder=Recorder(FailingStream(fail_close=True)),
    )
    session.start()
    session.wait(WAIT_SECONDS)
    assert session.status.state is State.FAILED
    assert "input/output error" in session.status.detail
