# SPDX-License-Identifier: MIT
import pytest

from pico_gui.commands import COMMAND_LIMIT, CommandError, command_bytes


def test_a_command_line_ends_in_a_line_feed() -> None:
    assert command_bytes("run 1000") == b"run 1000\n"


def test_sixty_four_characters_are_accepted() -> None:
    line = "a" * COMMAND_LIMIT
    assert command_bytes(line) == (line + "\n").encode("ascii")


def test_a_line_longer_than_the_limit_is_refused() -> None:
    with pytest.raises(CommandError, match="longer than 64"):
        command_bytes("a" * (COMMAND_LIMIT + 1))


def test_an_empty_line_is_refused() -> None:
    with pytest.raises(CommandError, match="empty"):
        command_bytes("")


def test_a_line_feed_inside_the_line_is_refused() -> None:
    with pytest.raises(CommandError, match="not printable ASCII"):
        command_bytes("run 10\n0")


def test_a_character_outside_ascii_is_refused() -> None:
    with pytest.raises(CommandError, match="not printable ASCII"):
        command_bytes("cfg 05 µs")


def test_the_refusal_names_the_position_of_the_bad_character() -> None:
    with pytest.raises(CommandError, match="position 8"):
        command_bytes("cfg 05 \t0")
