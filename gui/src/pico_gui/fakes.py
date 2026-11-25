# SPDX-License-Identifier: MIT
"""Fakes for the transport seams, used by tests and by hardware-free runs."""

from pico_tooling.device_errors import RefusalError
from pico_tooling.device_proxy import Board

from pico_gui.transport import Connection


class FakeConnection:
    """A `Connection` that returns scripted chunks, then reports a closed stream."""

    def __init__(self, chunks: list[bytes]) -> None:
        self._chunks = list(chunks)
        self.sent = bytearray()
        self.timeout: float | None = None
        self.shut_down = False
        self.closed = False

    def recv(self, size: int, /) -> bytes:
        if not self._chunks:
            return b""
        chunk = self._chunks.pop(0)
        if len(chunk) > size:
            self._chunks.insert(0, chunk[size:])
            return chunk[:size]
        return chunk

    def sendall(self, data: bytes, /) -> None:
        self.sent += data

    def settimeout(self, value: float | None, /) -> None:
        self.timeout = value

    def shutdown(self, _how: int, /) -> None:
        self.shut_down = True

    def close(self) -> None:
        self.closed = True


class FakeProxy:
    """A `ProxyClient` over a fixed set of boards and one scripted stream."""

    def __init__(
        self,
        boards: list[Board],
        *,
        allowed: tuple[str, ...] = (),
        stream: list[bytes] | None = None,
    ) -> None:
        self._boards = boards
        self._allowed = allowed
        self._stream = [] if stream is None else stream
        self.opened: list[tuple[str, int, str]] = []
        self.connection: FakeConnection | None = None

    def list(self) -> list[Board]:
        return list(self._boards)

    def allowed(self, serial: str) -> None:
        if serial not in self._allowed:
            raise RefusalError(
                2,
                f"{serial} is not in the allow-list kept on the host",
                code="denied",
            )

    def open_serial(self, serial: str, baud: int, port: str) -> Connection:
        self.opened.append((serial, baud, port))
        self.connection = FakeConnection(self._stream)
        return self.connection
