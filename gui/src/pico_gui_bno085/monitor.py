# SPDX-License-Identifier: MIT
"""Bind the line stream to the model from the reader thread."""

import threading
from collections.abc import Callable

from pico_gui_bno085.model import Model, Snapshot
from pico_gui_bno085.protocol import ProtocolError, parse_line


class Monitor:
    """Parse lines into a `Model` under a lock, so a UI can read it meanwhile."""

    def __init__(self, clock: Callable[[], float], model: Model | None = None) -> None:
        self._clock = clock
        self._model = Model() if model is None else model
        self._lock = threading.Lock()

    def on_line(self, text: str, now: float) -> None:
        """Take one line from the reader thread; a refused line is counted."""
        try:
            line = parse_line(text)
        except ProtocolError:
            with self._lock:
                self._model.reject()
            return
        with self._lock:
            self._model.feed(line, now)

    def snapshot(self) -> Snapshot:
        """Return the model's state now."""
        now = self._clock()
        with self._lock:
            return self._model.snapshot(now)
