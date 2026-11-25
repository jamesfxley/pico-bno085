# SPDX-License-Identifier: MIT
"""Write received lines in the device's own line format."""

from pathlib import Path
from types import TracebackType
from typing import Self, TextIO

from pico_gui.transport import TransportError


class Recorder:
    """Append lines to a file, one per line, exactly as the device sent them.

    The file needs no conversion to serve as a replay or a test fixture.
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
        self._stream.write(line + "\n")

    def close(self) -> None:
        self._stream.close()

    def __enter__(self) -> Self:
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        self.close()
