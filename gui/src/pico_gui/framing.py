# SPDX-License-Identifier: MIT
"""Split a byte stream into text lines."""

import re

DEFAULT_LINE_LIMIT = 2048

_TERMINATOR = re.compile(rb"[\r\n]")


class LineFramer:
    """Collect bytes and return each complete line as text.

    A line ends at a line feed or a carriage return, and an empty line is
    dropped, so a CR LF pair is one terminator. Bytes outside ASCII decode as
    U+FFFD. A line longer than `limit` bytes is discarded whole and counted in
    `dropped`, which bounds the buffer when a peer never sends a terminator.
    """

    def __init__(self, limit: int = DEFAULT_LINE_LIMIT) -> None:
        if limit < 1:
            raise ValueError("the line limit must be at least 1")
        self._limit = limit
        self._pending = bytearray()
        self._overflow = False
        self.dropped = 0

    def feed(self, data: bytes) -> list[str]:
        """Return the lines completed by `data`, in order."""
        lines: list[str] = []
        parts = _TERMINATOR.split(data)
        for index, part in enumerate(parts):
            self._append(part)
            if index < len(parts) - 1:
                line = self._take()
                if line:
                    lines.append(line)
        return lines

    def finish(self) -> list[str]:
        """Return the unterminated last line at the end of a stream, if any."""
        line = self._take()
        return [line] if line else []

    def _append(self, part: bytes) -> None:
        if self._overflow:
            return
        self._pending += part
        if len(self._pending) > self._limit:
            self._overflow = True
            self._pending.clear()

    def _take(self) -> str:
        if self._overflow:
            self._overflow = False
            self.dropped += 1
            return ""
        text = bytes(self._pending).decode("ascii", errors="replace")
        self._pending.clear()
        return text
