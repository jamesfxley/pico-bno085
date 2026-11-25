# SPDX-License-Identifier: MIT
"""The shared look of every family GUI."""

from nicegui import ui

from pico_gui.session import State

PRIMARY = "#3b82f6"
STATE_COLOURS = {
    State.IDLE: "grey",
    State.CONNECTED: "positive",
    State.ENDED: "info",
    State.FAILED: "negative",
}


def apply_theme() -> None:
    """Set the dark scheme and the accent colour for the current page."""
    ui.dark_mode(value=True)
    ui.colors(primary=PRIMARY)
