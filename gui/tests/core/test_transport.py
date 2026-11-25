# SPDX-License-Identifier: MIT
import socket

import pytest

from pico_gui.fakes import FakeConnection
from pico_gui.transport import SocketTransport, TransportError


class BrokenConnection(FakeConnection):
    def recv(self, _size: int, /) -> bytes:
        raise ConnectionResetError("reset by peer")

    def sendall(self, _data: bytes, /) -> None:
        raise BrokenPipeError("pipe closed")

    def shutdown(self, _how: int, /) -> None:
        raise OSError("not connected")


class IdleConnection(FakeConnection):
    def recv(self, _size: int, /) -> bytes:
        raise TimeoutError


def test_the_poll_interval_becomes_the_socket_timeout() -> None:
    connection = FakeConnection([])
    SocketTransport(connection, poll_seconds=0.5)
    assert connection.timeout == 0.5


def test_recv_returns_the_bytes_and_then_an_empty_end_of_stream() -> None:
    transport = SocketTransport(FakeConnection([b"ab", b"c"]))
    assert transport.recv(16) == b"ab"
    assert transport.recv(16) == b"c"
    assert transport.recv(16) == b""


def test_send_reaches_the_connection() -> None:
    connection = FakeConnection([])
    SocketTransport(connection).send(b"payload 1\n")
    assert bytes(connection.sent) == b"payload 1\n"


def test_a_timeout_passes_through_so_the_caller_can_check_for_a_stop() -> None:
    with pytest.raises(TimeoutError):
        SocketTransport(IdleConnection([])).recv(16)


def test_a_failed_read_becomes_a_transport_error_with_its_cause() -> None:
    with pytest.raises(TransportError, match="reset by peer") as caught:
        SocketTransport(BrokenConnection([])).recv(16)
    assert isinstance(caught.value.__cause__, ConnectionResetError)


def test_a_failed_write_becomes_a_transport_error() -> None:
    with pytest.raises(TransportError, match="pipe closed"):
        SocketTransport(BrokenConnection([])).send(b"x")


def test_close_shuts_down_then_closes() -> None:
    connection = FakeConnection([])
    SocketTransport(connection).close()
    assert connection.shut_down
    assert connection.closed


def test_close_releases_the_socket_even_when_shutdown_fails() -> None:
    connection = BrokenConnection([])
    SocketTransport(connection).close()
    assert connection.closed


def test_a_real_socket_pair_carries_bytes_and_reports_the_end() -> None:
    near, far = socket.socketpair()
    transport = SocketTransport(near)
    far.sendall(b"E,5,0,1,3\n")
    far.close()
    assert transport.recv(64) == b"E,5,0,1,3\n"
    assert transport.recv(64) == b""
    transport.close()


def test_a_real_socket_without_data_times_out() -> None:
    near, far = socket.socketpair()
    transport = SocketTransport(near, poll_seconds=0.01)
    try:
        with pytest.raises(TimeoutError):
            transport.recv(64)
    finally:
        transport.close()
        far.close()
