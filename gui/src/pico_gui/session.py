# SPDX-License-Identifier: MIT
"""Read a transport on a thread and deliver its lines."""

import threading
from collections.abc import Callable
from dataclasses import dataclass
from enum import Enum

from pico_gui.framing import DEFAULT_LINE_LIMIT, LineFramer
from pico_gui.recorder import Recorder
from pico_gui.transport import Transport, TransportError

CHUNK_BYTES = 4096
JOIN_SECONDS = 5.0


class State(Enum):
    """Where a session is in its life."""

    IDLE = "idle"
    CONNECTED = "connected"
    ENDED = "ended"
    FAILED = "failed"


@dataclass(frozen=True)
class Status:
    """A session's state, the reason for a failure and its line counts."""

    state: State
    detail: str = ""
    lines: int = 0
    dropped: int = 0


class Session:
    """Frame a transport's bytes and pass each line, with its arrival time, on.

    `on_line` runs on the reader thread. `status` is safe to read from any
    thread.
    """

    def __init__(
        self,
        transport: Transport,
        on_line: Callable[[str, float], None],
        *,
        clock: Callable[[], float],
        recorder: Recorder | None = None,
        line_limit: int = DEFAULT_LINE_LIMIT,
    ) -> None:
        self._transport = transport
        self._on_line = on_line
        self._clock = clock
        self._recorder = recorder
        self._framer = LineFramer(line_limit)
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._lines = 0
        self._status = Status(State.IDLE)

    @property
    def status(self) -> Status:
        return self._status

    def start(self) -> None:
        """Start the reader thread."""
        if self._thread is not None:
            raise RuntimeError("the session has started")
        self._thread = threading.Thread(target=self.run, name="session", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        """Stop reading, close the transport and the recording, and wait."""
        self._stop.set()
        self._transport.close()
        self.wait(JOIN_SECONDS)

    def wait(self, timeout: float | None = None) -> None:
        """Wait for the reader thread to finish, at most `timeout` seconds."""
        if self._thread is not None:
            self._thread.join(timeout)

    def _set(self, state: State, detail: str = "") -> None:
        self._status = Status(state, detail, self._lines, self._framer.dropped)

    def run(self) -> None:
        """Read until the stream ends or `stop` is called.

        This is the reader thread's entry, and a caller may run it directly to
        read on its own thread. An expected failure becomes the status
        (PY-03.3). Any other exception propagates with its traceback, and the
        `finally` clause records that the session failed.
        """
        self._set(State.CONNECTED)
        try:
            self._pump()
        except TransportError as error:
            if self._stop.is_set():
                self._set(State.ENDED)
            else:
                self._set(State.FAILED, str(error))
        else:
            self._set(State.ENDED)
        finally:
            if self._recorder is not None:
                self._recorder.close()
            if self._status.state is State.CONNECTED:
                self._set(State.FAILED, "the reader stopped unexpectedly")

    def _pump(self) -> None:
        while not self._stop.is_set():
            try:
                data = self._transport.recv(CHUNK_BYTES)
            except TimeoutError:
                continue
            if not data:
                break
            self._deliver(self._framer.feed(data))
        self._deliver(self._framer.finish())

    def _deliver(self, lines: list[str]) -> None:
        if not lines:
            return
        now = self._clock()
        for line in lines:
            if self._recorder is not None:
                self._recorder.write(line)
            self._on_line(line, now)
        self._lines += len(lines)
        self._set(State.CONNECTED)
