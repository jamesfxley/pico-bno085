# SPDX-License-Identifier: MIT
import io
import threading
from pathlib import Path

import pytest

from pico_gui.framing import LineFramer
from pico_gui.recorder import Recorder, RecorderSlot
from pico_gui.replay import ReplayTransport
from pico_gui.transport import TransportError

DATA_DIR = Path(__file__).resolve().parent.parent / "data"


def test_lines_are_written_one_per_line(tmp_path: Path) -> None:
    path = tmp_path / "out.txt"
    with Recorder.open(path) as recorder:
        recorder.write("# ready")
        recorder.write("E,5,0,100,3")
    assert path.read_bytes() == b"# ready\nE,5,0,100,3\n"


def test_a_line_outside_ascii_is_written_with_a_replacement(tmp_path: Path) -> None:
    path = tmp_path / "out.txt"
    with Recorder.open(path) as recorder:
        recorder.write("a�b")
    assert path.read_bytes() == b"a?b\n"


def test_a_recording_replays_to_the_lines_that_were_written(tmp_path: Path) -> None:
    lines = (DATA_DIR / "session.txt").read_text(encoding="ascii").splitlines()
    path = tmp_path / "round-trip.txt"
    with Recorder.open(path) as recorder:
        for line in lines:
            recorder.write(line)
    assert path.read_bytes() == (DATA_DIR / "session.txt").read_bytes()
    framer = LineFramer()
    replay = ReplayTransport.from_path(path)
    replayed: list[str] = []
    while chunk := replay.recv(100):
        replayed += framer.feed(chunk)
    replay.close()
    assert replayed == lines


def test_an_unwritable_path_is_a_transport_error(tmp_path: Path) -> None:
    with pytest.raises(TransportError, match="cannot write the recording"):
        Recorder.open(tmp_path / "missing" / "out.txt")


class FailingStream(io.StringIO):
    """A stream whose write, flush or close fails as a full disk would."""

    def __init__(
        self,
        *,
        fail_write: bool = False,
        fail_flush: bool = False,
        fail_close: bool = False,
    ) -> None:
        super().__init__()
        self._fail_write = fail_write
        self._fail_flush = fail_flush
        self._fail_close = fail_close

    def flush(self) -> None:
        if self._fail_flush:
            raise OSError("no space left on device")
        super().flush()

    def write(self, text: str, /) -> int:
        if self._fail_write:
            raise OSError("no space left on device")
        return super().write(text)

    def close(self) -> None:
        if self._fail_close:
            # Fail once; the finaliser's later call must not fail again.
            self._fail_close = False
            raise OSError("input/output error")
        super().close()


def test_a_failed_write_is_a_transport_error() -> None:
    recorder = Recorder(FailingStream(fail_write=True))
    with pytest.raises(TransportError, match="no space left on device"):
        recorder.write("# ready")


def test_a_failed_flush_is_a_transport_error() -> None:
    recorder = Recorder(FailingStream(fail_flush=True))
    with pytest.raises(TransportError, match="no space left on device"):
        recorder.write("# ready")


def test_a_failed_close_is_a_transport_error() -> None:
    recorder = Recorder(FailingStream(fail_close=True))
    with pytest.raises(TransportError, match="input/output error"):
        recorder.close()


def test_a_write_after_close_is_a_transport_error() -> None:
    recorder = Recorder(io.StringIO())
    recorder.close()
    with pytest.raises(TransportError, match="cannot write the recording"):
        recorder.write("# ready")


def test_each_line_is_on_disk_before_the_file_is_closed(tmp_path: Path) -> None:
    path = tmp_path / "out.txt"
    recorder = Recorder.open(path)
    recorder.write("# ready")
    assert path.read_bytes() == b"# ready\n"
    recorder.close()


def test_a_slot_writes_to_the_attached_recorder_only(tmp_path: Path) -> None:
    slot = RecorderSlot()
    slot.write("# ready")
    first = Recorder.open(tmp_path / "first.txt")
    slot.attach(first)
    slot.write("E,5,0,100,3")
    second = Recorder.open(tmp_path / "second.txt")
    slot.attach(second)
    slot.write("E,5,1,200,3")
    assert (tmp_path / "first.txt").read_bytes() == b"E,5,0,100,3\n"
    assert (tmp_path / "second.txt").read_bytes() == b"E,5,1,200,3\n"
    first.close()
    detached = slot.detach()
    assert detached is not None
    detached.close()


def test_a_slot_reports_its_recorder_and_detaches_it(tmp_path: Path) -> None:
    slot = RecorderSlot()
    attached = Recorder.open(tmp_path / "out.txt")
    assert slot.detach() is None
    slot.attach(attached)
    assert slot.attached
    detached = slot.detach()
    assert detached is attached
    assert slot.detach() is None
    detached.close()


def test_a_failed_write_through_a_slot_without_a_reporter_raises() -> None:
    slot = RecorderSlot(Recorder(FailingStream(fail_write=True)))
    with pytest.raises(TransportError, match="no space left on device"):
        slot.write("# ready")


def test_a_failed_write_through_a_slot_with_a_reporter_detaches() -> None:
    failures: list[str] = []
    slot = RecorderSlot(
        Recorder(FailingStream(fail_write=True)),
        on_failure=failures.append,
    )
    slot.write("# ready")
    slot.write("# ready")
    assert len(failures) == 1
    assert "no space left on device" in failures[0]
    assert not slot.attached


def test_a_failed_recorder_is_closed_before_it_is_detached() -> None:
    stream = FailingStream(fail_write=True)
    failures: list[str] = []
    slot = RecorderSlot(Recorder(stream), on_failure=failures.append)
    slot.write("# ready")
    assert stream.closed
    assert "no space left on device" in failures[0]


def test_a_failed_recorder_that_cannot_close_still_reports_the_write() -> None:
    failures: list[str] = []
    slot = RecorderSlot(
        Recorder(FailingStream(fail_write=True, fail_close=True)),
        on_failure=failures.append,
    )
    slot.write("# ready")
    assert len(failures) == 1
    assert "no space left on device" in failures[0]
    assert not slot.attached


def test_an_existing_file_is_refused_and_left_as_it_is(tmp_path: Path) -> None:
    path = tmp_path / "out.txt"
    path.write_bytes(b"# ready\n")
    with pytest.raises(TransportError, match="exists") as caught:
        Recorder.open(path)
    assert str(path) in str(caught.value)
    assert path.read_bytes() == b"# ready\n"


def test_a_slot_reports_attached_while_a_write_holds_its_lock() -> None:
    entered = threading.Event()
    release = threading.Event()

    class StalledStream(io.StringIO):
        def write(self, text: str, /) -> int:
            entered.set()
            release.wait(5.0)
            return super().write(text)

    slot = RecorderSlot(Recorder(StalledStream()))
    writer = threading.Thread(target=slot.write, args=("# ready",))
    writer.start()
    answers: list[bool] = []
    probe = threading.Thread(target=lambda: answers.append(slot.attached))
    try:
        assert entered.wait(5.0)
        probe.start()
        probe.join(1.0)
        assert answers == [True]
    finally:
        release.set()
        writer.join(5.0)
        probe.join(5.0)
