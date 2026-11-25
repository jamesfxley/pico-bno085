# SPDX-License-Identifier: MIT
import argparse

import pytest

from pico_gui.launch import (
    DEFAULT_HOST,
    DEFAULT_PORT,
    add_server_arguments,
    forwarding_steps,
    port_number,
    shutdown_message,
)


def parse(*argv: str) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    add_server_arguments(parser)
    return parser.parse_args(argv)


def test_the_defaults_listen_on_loopback() -> None:
    args = parse()
    assert (args.host, args.port) == (DEFAULT_HOST, DEFAULT_PORT)
    assert DEFAULT_HOST == "127.0.0.1"


def test_port_selects_the_server_port() -> None:
    assert parse("--port", "9100").port == 9100


@pytest.mark.parametrize("text", ["0", "65536", "-1", "http", ""])
def test_a_bad_port_is_refused(text: str) -> None:
    with pytest.raises(argparse.ArgumentTypeError):
        port_number(text)


def test_the_steps_name_the_exact_port_and_how_to_stop() -> None:
    text = forwarding_steps(9100)
    assert "forward port 9100" in text
    assert "Forward a Port" in text
    assert "remove port 9100" in text
    assert "Ctrl+C" in text


def test_the_steps_mark_what_the_documentation_does_not_state() -> None:
    assert "UNCONFIRMED" in forwarding_steps(8080)


def test_the_shutdown_message_says_the_forward_can_be_removed() -> None:
    assert "9100" in shutdown_message(9100)
    assert "can now be removed" in shutdown_message(9100)
