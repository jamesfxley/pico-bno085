# SPDX-License-Identifier: MIT
from pathlib import Path

import pytest
from pico_tooling.device_proxy import Board

from pico_gui.device import CONSOLE_BAUD, choose_port, connect_proxy, open_device
from pico_gui.fakes import FakeProxy
from pico_gui.transport import TransportError

SERIAL = "0123456789ABCDEF"


def board(*ports: str, serial: str = SERIAL) -> Board:
    return Board(serial, "Pico", ports)


def test_open_device_checks_the_allow_list_then_opens_the_only_port() -> None:
    proxy = FakeProxy([board("PORT-A")], allowed=(SERIAL,), stream=[b"# ready\n"])
    transport = open_device(SERIAL, proxy_factory=lambda _start: proxy)
    assert proxy.opened == [(SERIAL, CONSOLE_BAUD, "PORT-A")]
    assert transport.recv(64) == b"# ready\n"
    assert transport.recv(64) == b""


def test_an_explicit_port_skips_the_board_listing() -> None:
    proxy = FakeProxy([], allowed=(SERIAL,))
    open_device(SERIAL, port="PORT-C", baud=9600, proxy_factory=lambda _start: proxy)
    assert proxy.opened == [(SERIAL, 9600, "PORT-C")]


def test_the_factory_receives_the_start_directory(tmp_path: Path) -> None:
    seen: list[Path] = []

    def factory(start: Path) -> FakeProxy:
        seen.append(start)
        return FakeProxy([board("PORT-A")], allowed=(SERIAL,))

    open_device(SERIAL, start=tmp_path, proxy_factory=factory)
    assert seen == [tmp_path]


def test_a_board_missing_from_the_allow_list_is_a_transport_error() -> None:
    proxy = FakeProxy([board("PORT-A")], allowed=())
    with pytest.raises(TransportError, match="allow-list") as caught:
        open_device(SERIAL, proxy_factory=lambda _start: proxy)
    assert caught.value.__cause__ is not None
    assert proxy.opened == []


def test_a_board_that_is_not_listed_is_a_transport_error() -> None:
    proxy = FakeProxy([board("PORT-A", serial="OTHER")], allowed=(SERIAL,))
    with pytest.raises(TransportError, match="not connected"):
        open_device(SERIAL, proxy_factory=lambda _start: proxy)


def test_several_ports_ask_the_caller_to_choose() -> None:
    with pytest.raises(TransportError, match="PORT-A, PORT-B"):
        choose_port([board("PORT-A", "PORT-B")], SERIAL)


def test_no_ports_is_a_transport_error() -> None:
    with pytest.raises(TransportError, match="none"):
        choose_port([board()], SERIAL)


def test_a_missing_connection_file_names_what_to_start(tmp_path: Path) -> None:
    with pytest.raises(TransportError, match="proxy"):
        connect_proxy(tmp_path)


def test_a_connection_file_gives_a_proxy_without_contacting_it(tmp_path: Path) -> None:
    (tmp_path / ".pico-device-proxy.json").write_text(
        '{"host": "172.20.0.1", "port": 47600, "token": "t"}',
        encoding="utf-8",
    )
    client = connect_proxy(tmp_path)
    assert callable(client.open_serial)
