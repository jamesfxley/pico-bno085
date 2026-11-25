# SPDX-License-Identifier: MIT
"""Replay a recording as if a device sent it."""

from collections.abc import Callable
from pathlib import Path
from typing import BinaryIO, Self

from pico_gui.transport import TransportError


class ReplayTransport:
    """A read-only transport over a recording in the device's own line format.

    Without `lines_per_second` the recording is returned in chunks as fast as it
    is read. With it, each call returns one line after `sleep` has waited the
    line's share of a second, so a live view sees a steady feed.
    """

    def __init__(
        self,
        stream: BinaryIO,
        *,
        lines_per_second: float | None = None,
        sleep: Callable[[float], None] | None = None,
    ) -> None:
        if lines_per_second is not None and lines_per_second <= 0:
            raise ValueError("lines_per_second must be positive")
        if lines_per_second is not None and sleep is None:
            raise ValueError("a paced replay needs a sleep function")
        self._stream = stream
        self._interval = None if lines_per_second is None else 1 / lines_per_second
        self._sleep = sleep
        self._closed = False

    @classmethod
    def from_path(
        cls,
        path: Path,
        *,
        lines_per_second: float | None = None,
        sleep: Callable[[float], None] | None = None,
    ) -> Self:
        """Open the recording at `path`; a missing file is a `TransportError`."""
        try:
            stream = path.open("rb")
        except OSError as error:
            raise TransportError(
                f"cannot open the recording {path}: {error}",
            ) from error
        return cls(stream, lines_per_second=lines_per_second, sleep=sleep)

    def recv(self, size: int, /) -> bytes:
        if self._closed:
            return b""
        if self._interval is None or self._sleep is None:
            return self._stream.read(size)
        data = self._stream.readline(size)
        if data:
            self._sleep(self._interval)
        return data

    def send(self, _data: bytes, /) -> None:
        raise TransportError("a replay is read-only")

    def close(self) -> None:
        self._closed = True
        self._stream.close()
