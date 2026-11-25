# SPDX-License-Identifier: MIT
import pytest

from pico_gui.framing import LineFramer


def test_lines_split_at_line_feed() -> None:
    assert LineFramer().feed(b"a\nbc\n") == ["a", "bc"]


def test_a_carriage_return_line_feed_pair_is_one_terminator() -> None:
    assert LineFramer().feed(b"a\r\nb\rc\n") == ["a", "b", "c"]


def test_empty_lines_are_dropped() -> None:
    assert LineFramer().feed(b"\n\r\n\na\n\n") == ["a"]


def test_a_line_split_across_chunks_is_joined() -> None:
    framer = LineFramer()
    assert framer.feed(b"hel") == []
    assert framer.feed(b"lo\nwor") == ["hello"]
    assert framer.feed(b"ld\n") == ["world"]


def test_finish_returns_an_unterminated_last_line_once() -> None:
    framer = LineFramer()
    framer.feed(b"tail")
    assert framer.finish() == ["tail"]
    assert framer.finish() == []


def test_bytes_outside_ascii_decode_as_replacement_characters() -> None:
    assert LineFramer().feed(b"a\xffb\n") == ["a�b"]


def test_an_overlong_line_is_dropped_whole_and_counted() -> None:
    framer = LineFramer(limit=4)
    lines = framer.feed(b"ok\ntoolong")
    lines += framer.feed(b"still\nfine\n")
    assert lines == ["ok", "fine"]
    assert framer.dropped == 1


def test_a_line_of_exactly_the_limit_is_kept() -> None:
    framer = LineFramer(limit=4)
    assert framer.feed(b"abcd\n") == ["abcd"]
    assert framer.dropped == 0


def test_the_buffer_stays_bounded_without_a_terminator() -> None:
    framer = LineFramer(limit=8)
    for _ in range(100):
        framer.feed(b"x" * 64)
    assert framer.feed(b"\n") == []
    assert framer.dropped == 1


def test_a_limit_below_one_is_refused() -> None:
    with pytest.raises(ValueError, match="at least 1"):
        LineFramer(limit=0)
