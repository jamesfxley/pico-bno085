# SPDX-License-Identifier: MIT
from pathlib import Path

import pytest

from pico_gui.framing import LineFramer
from pico_gui.recorder import Recorder
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
