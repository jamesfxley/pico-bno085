# SPDX-License-Identifier: MIT
"""Shell components shared by every family GUI."""

from nicegui import ui

from pico_gui.session import Status
from pico_gui.theme import STATE_COLOURS

REFRESH_HZ = 30
REFRESH_INTERVAL_SECONDS = 1 / REFRESH_HZ

__all__ = (
    "REFRESH_HZ",
    "REFRESH_INTERVAL_SECONDS",
    "ConnectionBar",
    "StatusLine",
    "describe",
)


def describe(status: Status) -> str:
    """Return the connection state as one line of text."""
    text = status.state.value
    if status.detail:
        text += f": {status.detail}"
    text += f" ({status.lines} lines"
    if status.dropped:
        text += f", {status.dropped} too long and dropped"
    return text + ")"


class ConnectionBar:
    """A header bar naming the data source and the connection state."""

    def __init__(self, source: str) -> None:
        with ui.header().classes("items-center"):
            ui.label(source).classes("text-lg")
            ui.space()
            self._badge = ui.badge("idle")

    def update(self, status: Status) -> None:
        self._badge.set_text(describe(status))
        self._badge.props(f"color={STATE_COLOURS[status.state]}")


class StatusLine:
    """One line of text below the content."""

    def __init__(self) -> None:
        self._label = ui.label().classes("text-sm text-grey")

    def update(self, text: str) -> None:
        self._label.set_text(text)
