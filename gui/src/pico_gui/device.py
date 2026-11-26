# SPDX-License-Identifier: MIT
"""Open a board's serial stream through the device proxy.

This is the only module that imports `pico_tooling.device_proxy`. Every other
module reaches a device through `Transport`.
"""

import time
from collections.abc import Callable
from pathlib import Path
from typing import Protocol

from pico_tooling.device_errors import RefusalError
from pico_tooling.device_proxy import Board, Proxy, find_connection

from pico_gui.transport import Connection, SocketTransport, TransportError

__all__ = (
    "CONSOLE_BAUD",
    "ProxyClient",
    "ProxyFactory",
    "choose_port",
    "connect_proxy",
    "open_device",
)

CONSOLE_BAUD = 115200


class ProxyClient(Protocol):
    """The part of `pico_tooling.device_proxy.Proxy` that this module uses."""

    def list(self) -> list[Board]: ...

    def allowed(self, serial: str) -> None: ...

    def open_serial(self, serial: str, baud: int, port: str) -> Connection: ...


ProxyFactory = Callable[[Path], ProxyClient]


class _Host:
    """The clock `Proxy` reads when a deadline is set; none is set here."""

    @staticmethod
    def clock() -> float:
        return time.monotonic()


def connect_proxy(start: Path) -> ProxyClient:
    """Return a client for the proxy whose connection file lies above `start`."""
    found = find_connection(start)
    if found is None:
        raise TransportError(
            f"no valid .pico-device-proxy.json above {start}; start the device "
            "proxy on Windows before opening the container",
        )
    path, data = found
    return Proxy(_Host(), path, data)


def choose_port(boards: list[Board], serial: str) -> str:
    """Return the one port of the board `serial`, or raise `TransportError`."""
    board = next((board for board in boards if board.serial == serial), None)
    if board is None:
        raise TransportError(f"board {serial} is not connected")
    if len(board.ports) != 1:
        found = ", ".join(board.ports) if board.ports else "none"
        raise TransportError(
            f"board {serial} has ports: {found}; name the one to open",
        )
    return board.ports[0]


def open_device(
    serial: str,
    *,
    port: str | None = None,
    baud: int = CONSOLE_BAUD,
    start: Path | None = None,
    proxy_factory: ProxyFactory = connect_proxy,
) -> SocketTransport:
    """Open the board's serial port and return its transport.

    A refusal by the proxy, an unlisted board and a missing connection file
    are expected failures and raise `TransportError`.
    """
    proxy = proxy_factory(Path.cwd() if start is None else start)
    try:
        proxy.allowed(serial)
        chosen = port if port is not None else choose_port(proxy.list(), serial)
        connection = proxy.open_serial(serial, baud, chosen)
    except RefusalError as error:
        raise TransportError(str(error)) from error
    return SocketTransport(connection)
