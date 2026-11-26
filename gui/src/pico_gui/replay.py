# SPDX-License-Identifier: MIT
"""Replay a recording as if a device sent it."""

import os
from collections.abc import Callable
from pathlib import Path
from typing import BinaryIO, Self

from pico_gui.transport import TransportError

PAUSE_POLL_SECONDS = 0.05

__all__ = ("PAUSE_POLL_SECONDS", "ReplayTransport")


class ReplayTransport:
    """A read-only transport over a recording in the device's own line format.

    Without `lines_per_second` the recording is returned in chunks as fast as
    it is read. With it, each call returns one line after `sleep` has waited
    the line's share of a second, so a live view sees a steady feed.

    `pause` makes `recv` wait without consuming the recording, so the session
    stays open while the replay is held. `progress` is the fraction of the
    recording read.
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
        self._paused = False
        end = stream.seek(0, os.SEEK_END)
        stream.seek(0)
        self._total_bytes = end
        self._read_bytes = 0

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
        while self._paused and not self._closed:
            if self._sleep is None:
                # An unpaced replay has no way to wait, so it cannot be held.
                self._paused = False
                break
            self._sleep(PAUSE_POLL_SECONDS)
        if self._closed:
            return b""
        if self._interval is None or self._sleep is None:
            data = self._stream.read(size)
        else:
            data = self._stream.readline(size)
            if data:
                self._sleep(self._interval)
        self._read_bytes += len(data)
        return data

    def send(self, _data: bytes, /) -> None:
        raise TransportError("a replay is read-only")

    def close(self) -> None:
        self._closed = True
        self._stream.close()

    def pause(self) -> None:
        """Hold the replay where it stands; the session stays open."""
        self._paused = True

    def resume(self) -> None:
        """Continue a held replay."""
        self._paused = False

    def is_paused(self) -> bool:
        """Return whether the replay is held."""
        return self._paused

    def progress(self) -> float:
        """Return the fraction of the recording read, from 0 to 1."""
        if self._total_bytes == 0:
            return 1.0
        return min(1.0, self._read_bytes / self._total_bytes)
