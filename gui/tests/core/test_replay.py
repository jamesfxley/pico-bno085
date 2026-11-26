# SPDX-License-Identifier: MIT
import io
from pathlib import Path

import pytest

from pico_gui.replay import PAUSE_POLL_SECONDS, ReplayTransport
from pico_gui.transport import TransportError

DATA = b"# ready\nE,5,0,100,3\nE,5,1,200,3\n"


def drain(transport: ReplayTransport, size: int = 8) -> list[bytes]:
    chunks = []
    while chunk := transport.recv(size):
        chunks.append(chunk)
    return chunks


def test_an_unpaced_replay_returns_the_bytes_in_chunks() -> None:
    chunks = drain(ReplayTransport(io.BytesIO(DATA)))
    assert b"".join(chunks) == DATA
    assert all(len(chunk) <= 8 for chunk in chunks)


def test_a_paced_replay_returns_one_line_and_waits_for_each() -> None:
    waits: list[float] = []
    transport = ReplayTransport(
        io.BytesIO(DATA),
        lines_per_second=4,
        sleep=waits.append,
    )
    assert drain(transport, 64) == [b"# ready\n", b"E,5,0,100,3\n", b"E,5,1,200,3\n"]
    assert waits == [0.25, 0.25, 0.25]


def test_a_paced_replay_needs_a_sleep_function() -> None:
    with pytest.raises(ValueError, match="sleep"):
        ReplayTransport(io.BytesIO(DATA), lines_per_second=10)


def test_a_non_positive_rate_is_refused() -> None:
    stream = io.BytesIO(DATA)
    with pytest.raises(ValueError, match="positive"):
        ReplayTransport(stream, lines_per_second=0, sleep=lambda _s: None)


def test_a_replay_is_read_only() -> None:
    with pytest.raises(TransportError, match="read-only"):
        ReplayTransport(io.BytesIO(DATA)).send(b"tare 7 0\n")


def test_close_ends_the_stream_and_closes_the_file() -> None:
    stream = io.BytesIO(DATA)
    transport = ReplayTransport(stream)
    transport.close()
    assert transport.recv(8) == b""
    assert stream.closed


def test_from_path_reads_a_recording(tmp_path: Path) -> None:
    path = tmp_path / "session.txt"
    path.write_bytes(DATA)
    transport = ReplayTransport.from_path(path)
    assert b"".join(drain(transport)) == DATA
    transport.close()


def test_a_missing_recording_is_a_transport_error(tmp_path: Path) -> None:
    with pytest.raises(TransportError, match="cannot open the recording"):
        ReplayTransport.from_path(tmp_path / "absent.txt")


def test_progress_reaches_one_as_the_recording_is_read() -> None:
    transport = ReplayTransport(io.BytesIO(DATA))
    assert transport.progress() == 0.0
    transport.recv(8)
    assert 0.0 < transport.progress() < 1.0
    drain(transport)
    assert transport.progress() == 1.0


def test_the_progress_of_an_empty_recording_is_one() -> None:
    assert ReplayTransport(io.BytesIO(b"")).progress() == 1.0


def test_a_held_replay_waits_until_it_is_resumed() -> None:
    waits: list[float] = []

    def sleep(seconds: float) -> None:
        waits.append(seconds)
        transport.resume()

    transport = ReplayTransport(
        io.BytesIO(DATA),
        lines_per_second=4,
        sleep=sleep,
    )
    transport.pause()
    # The hold ends inside the first poll; the pace of the line follows it.
    assert transport.recv(64) == b"# ready\n"
    assert waits[:1] == [PAUSE_POLL_SECONDS]
    assert waits[1] == 0.25


def test_a_held_replay_ends_when_the_transport_closes() -> None:
    def sleep(_seconds: float) -> None:
        transport.close()

    transport = ReplayTransport(
        io.BytesIO(DATA),
        lines_per_second=4,
        sleep=sleep,
    )
    transport.pause()
    assert transport.recv(64) == b""


def test_a_held_unpaced_replay_cannot_wait_and_reads_on() -> None:
    transport = ReplayTransport(io.BytesIO(DATA))
    transport.pause()
    assert b"".join(drain(transport)) == DATA
    assert not transport.is_paused()


def test_resume_lets_a_held_replay_continue() -> None:
    transport = ReplayTransport(
        io.BytesIO(DATA),
        lines_per_second=4,
        sleep=lambda _s: None,
    )
    transport.pause()
    transport.resume()
    assert not transport.is_paused()
    assert drain(transport, 64) == [
        b"# ready\n",
        b"E,5,0,100,3\n",
        b"E,5,1,200,3\n",
    ]
