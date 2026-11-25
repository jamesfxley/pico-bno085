# SPDX-License-Identifier: MIT
"""The byte-stream seam between a device and the GUI."""

import contextlib
import socket
from typing import Protocol

DEFAULT_POLL_SECONDS = 0.2


class TransportError(Exception):
    """A transport could not open or lost its stream; the message names why."""


class Transport(Protocol):
    """A bidirectional byte stream.

    `recv` returns at most `size` bytes, and an empty result means the stream
    has ended. It raises `TimeoutError` when no byte arrived within the
    transport's poll interval, so a caller can check whether to stop.
    """

    def recv(self, size: int, /) -> bytes: ...

    def send(self, data: bytes, /) -> None: ...

    def close(self) -> None: ...


class Connection(Protocol):
    """The part of a socket that `SocketTransport` uses."""

    def recv(self, size: int, /) -> bytes: ...

    def sendall(self, data: bytes, /) -> None: ...

    def settimeout(self, value: float | None, /) -> None: ...

    def shutdown(self, how: int, /) -> None: ...

    def close(self) -> None: ...


class SocketTransport:
    """A transport over a connected socket, such as the device proxy's stream."""

    def __init__(
        self,
        connection: Connection,
        poll_seconds: float = DEFAULT_POLL_SECONDS,
    ) -> None:
        self._connection = connection
        self._connection.settimeout(poll_seconds)

    def recv(self, size: int, /) -> bytes:
        try:
            return self._connection.recv(size)
        except TimeoutError:
            raise
        except OSError as error:
            raise TransportError(f"the device stream failed: {error}") from error

    def send(self, data: bytes, /) -> None:
        try:
            self._connection.sendall(data)
        except OSError as error:
            raise TransportError(f"the device stream failed: {error}") from error

    def close(self) -> None:
        """Wake a blocked reader, then release the socket."""
        # The peer may already have closed the stream; the close below still
        # releases the socket.
        with contextlib.suppress(OSError):
            self._connection.shutdown(socket.SHUT_RDWR)
        self._connection.close()
