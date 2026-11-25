# SPDX-License-Identifier: MIT
from pico_gui.session import State, Status
from pico_gui.shell import REFRESH_HZ, REFRESH_INTERVAL_SECONDS, describe
from pico_gui.theme import STATE_COLOURS


def test_the_refresh_rate_is_at_most_thirty_hertz() -> None:
    assert REFRESH_HZ <= 30
    assert REFRESH_INTERVAL_SECONDS >= 1 / 30


def test_describe_names_state_and_counts() -> None:
    assert describe(Status(State.CONNECTED, "", 12, 0)) == "connected (12 lines)"


def test_describe_includes_a_failure_detail_and_dropped_lines() -> None:
    text = describe(Status(State.FAILED, "stream lost", 3, 2))
    assert text == "failed: stream lost (3 lines, 2 too long and dropped)"


def test_every_state_has_a_colour() -> None:
    assert set(STATE_COLOURS) == set(State)
