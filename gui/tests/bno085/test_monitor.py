# SPDX-License-Identifier: MIT
import io
from pathlib import Path

from pico_gui.replay import ReplayTransport
from pico_gui.session import Session, State
from pico_gui_bno085.monitor import Monitor

DATA = Path(__file__).resolve().parent.parent / "data" / "session.txt"
WAIT_SECONDS = 5.0


def test_a_parsed_line_reaches_the_model_and_a_refused_one_is_counted() -> None:
    monitor = Monitor(lambda: 1.0)
    monitor.on_line("E,5,0,100,3", 1.0)
    monitor.on_line("not a protocol line", 1.0)
    snapshot = monitor.snapshot()
    assert snapshot.sensors[0].count == 1
    assert snapshot.rejected == 1


def test_a_recorded_session_replays_into_the_model() -> None:
    monitor = Monitor(lambda: 0.0)
    session = Session(
        ReplayTransport(io.BytesIO(DATA.read_bytes())),
        monitor.on_line,
        clock=lambda: 0.0,
    )
    session.start()
    session.wait(WAIT_SECONDS)
    assert session.status.state is State.ENDED
    snapshot = monitor.snapshot()
    assert snapshot.rejected == 0
    assert [stats.sensor for stats in snapshot.sensors] == [
        1,
        2,
        3,
        4,
        5,
        6,
        7,
        8,
        9,
        15,
        40,
        41,
        42,
    ]
    by_sensor = {stats.sensor: stats for stats in snapshot.sensors}
    # LP-4: the G line prints sequence 0 every time; that is not loss.
    assert by_sensor[42].missing == 0
    assert by_sensor[42].count > 1
    assert snapshot.replies >= 8
