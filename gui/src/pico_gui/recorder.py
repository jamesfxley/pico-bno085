# SPDX-License-Identifier: MIT
"""Write received lines in the device's own line format."""

import contextlib
import threading
from collections.abc import Callable
from pathlib import Path
from types import TracebackType
from typing import Self, TextIO

from pico_gui.transport import TransportError

__all__ = ("Recorder", "RecorderSlot")


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
        """Create `path`; a failure is a `TransportError`.

        A file that exists is refused and left as it is, so a recording never
        destroys another one.
        """
        try:
            stream = path.open("x", encoding="ascii", errors="replace", newline="\n")
        except FileExistsError as error:
            raise TransportError(
                f"the recording {path} exists; it is not overwritten",
            ) from error
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


class RecorderSlot:
    """An optional recorder a session writes through, safe across threads.

    `write` runs on the reader thread while `attach` and `detach` run on the
    thread that drives the interface, so a write, an attach and a detach take
    the slot's lock. The write stays under it, because a detach that returns
    must mean no write is under way and none follows, so the caller can
    close the recorder; the cost is that a detach waits on a stalled disk.
    Without `on_failure` a write failure propagates to the session, which
    reports it in its status. With it, the failed recorder is detached, the
    session keeps reading and `on_failure` receives the reason.
    """

    def __init__(
        self,
        recorder: Recorder | None = None,
        *,
        on_failure: Callable[[str], None] | None = None,
    ) -> None:
        self._lock = threading.Lock()
        self._recorder = recorder
        self._on_failure = on_failure

    def attach(self, recorder: Recorder) -> None:
        """Start writing the session's lines to `recorder`."""
        with self._lock:
            self._recorder = recorder

    def detach(self) -> Recorder | None:
        """Stop writing and return the recorder, or `None` if none was attached."""
        with self._lock:
            recorder = self._recorder
            self._recorder = None
            return recorder

    @property
    def attached(self) -> bool:
        """Whether a recorder is attached.

        This reads one reference without the lock, so the interface's refresh
        never waits on a write that a stalled disk holds.
        """
        return self._recorder is not None

    def write(self, line: str) -> None:
        """Write `line` to the attached recorder, if one is attached.

        The write holds the slot's lock, so a recorder the interface detaches
        is never written after it has been closed. A write failure either
        propagates to the session or, with a reporter, closes and detaches
        the recorder and hands the reason to the reporter outside the lock.
        """
        failure: str | None = None
        report: Callable[[str], None] | None = None
        with self._lock:
            recorder = self._recorder
            if recorder is None:
                return
            try:
                recorder.write(line)
            except TransportError as error:
                if self._on_failure is None:
                    raise
                # The write already failed; the reason reported is its.
                with contextlib.suppress(TransportError):
                    recorder.close()
                self._recorder = None
                failure = str(error)
                report = self._on_failure
        if failure is not None and report is not None:
            report(failure)
