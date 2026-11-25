# SPDX-License-Identifier: MIT
"""Write received lines in the device's own line format."""

from pathlib import Path
from types import TracebackType
from typing import Self, TextIO

from pico_gui.transport import TransportError


class Recorder:
    """Write the lines a session delivers to a file, one per line.

    The lines are as `LineFramer` returns them, not the device's raw bytes:
    empty and overlong lines are absent, a CR LF pair is one line feed and a
    byte outside ASCII is written as `?`. Each line is flushed as it is
    written, so a killed process loses none. The file needs no conversion to
    serve as a replay or a test fixture. A failed write or close is a
    `TransportError`.
    """

    def __init__(self, stream: TextIO) -> None:
        self._stream = stream

    @classmethod
    def open(cls, path: Path) -> Self:
        """Create or truncate `path`; a failure is a `TransportError`."""
        try:
            stream = path.open("w", encoding="ascii", errors="replace", newline="\n")
        except OSError as error:
            raise TransportError(
                f"cannot write the recording {path}: {error}",
            ) from error
        return cls(stream)

    def write(self, line: str) -> None:
        """Write `line` and flush it."""
        try:
            self._stream.write(line + "\n")
            self._stream.flush()
        except (OSError, ValueError) as error:
            raise TransportError(f"cannot write the recording: {error}") from error

    def close(self) -> None:
        """Close the file."""
        try:
            self._stream.close()
        except OSError as error:
            raise TransportError(f"cannot close the recording: {error}") from error

    def __enter__(self) -> Self:
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        self.close()
