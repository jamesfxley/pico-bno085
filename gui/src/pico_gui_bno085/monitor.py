# SPDX-License-Identifier: MIT
"""Bind the line stream to the model from the reader thread."""

import threading
from collections.abc import Mapping

from pico_gui_bno085.model import Model, Snapshot
from pico_gui_bno085.protocol import ProtocolError, Telemetry, parse_line

__all__ = ("Monitor",)


class Monitor:
    """Parse lines into a `Model` under a lock, so a UI can read it meanwhile.

    `on_line` matches the session's line callback: it takes the line's
    arrival reading and ignores it, because every figure the model keeps
    comes from the event times the lines carry.
    """

    def __init__(self, model: Model | None = None) -> None:
        self._model = Model() if model is None else model
        self._lock = threading.Lock()

    def on_line(self, text: str, _now: float) -> None:
        """Take one line from the reader thread; a refused line is counted."""
        try:
            line = parse_line(text)
        except ProtocolError:
            with self._lock:
                self._model.reject()
            return
        with self._lock:
            self._model.feed(line, text)

    def snapshot(self, commanded: Mapping[int, int] | None = None) -> Snapshot:
        """Return the model's state, with each sensor's commanded interval."""
        with self._lock:
            return self._model.snapshot(commanded)

    def history(self, sensor: int) -> tuple[Telemetry, ...]:
        """Return the samples the model retains of `sensor`, oldest first."""
        with self._lock:
            return self._model.history(sensor)
