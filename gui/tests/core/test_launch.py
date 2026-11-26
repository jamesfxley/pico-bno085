# SPDX-License-Identifier: MIT
import argparse
import asyncio

import pytest

from pico_gui.launch import (
    DEFAULT_HOST,
    DEFAULT_PORT,
    Server,
    add_server_arguments,
    announce_when_started,
    bind_failure_message,
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


def test_the_steps_name_the_exact_port_and_the_documented_actions() -> None:
    text = forwarding_steps(9100)
    assert "listening on port 9100" in text
    assert "Command Palette (F1)" in text
    assert "Ports view" not in text
    assert "'Forward a Port'" in text
    assert "notification will tell you the localhost port" in text
    assert "for example http://localhost:4123" in text
    assert "temporary forward lasts for the duration of the VS Code session" in text
    assert "Ctrl+C" in text


def test_the_steps_state_nothing_that_the_documentation_does_not() -> None:
    text = forwarding_steps(8080)
    assert "UNCONFIRMED" not in text
    assert "remove" not in text.lower()
    # The page names no default local port, so the steps do not.
    assert "by default" not in text


def test_the_shutdown_message_is_plain() -> None:
    assert shutdown_message() == "Server stopped."


def test_the_bind_failure_names_the_address_and_the_remedy() -> None:
    message = bind_failure_message(Server("127.0.0.1", 9100))
    assert "127.0.0.1:9100" in message
    assert "--port" in message


def test_the_steps_are_said_only_after_the_server_has_started() -> None:
    said: list[str] = []
    polls = iter([False, False, True])

    def started() -> bool:
        value = next(polls)
        assert said == []
        return value

    async def attempt() -> None:
        announce_when_started(
            asyncio.get_running_loop(),
            started,
            said.append,
            9100,
            poll_seconds=0.001,
        )
        await asyncio.sleep(0.1)

    asyncio.run(attempt())
    assert said == [forwarding_steps(9100)]


def test_nothing_is_said_while_the_server_has_not_started() -> None:
    said: list[str] = []

    async def attempt() -> None:
        announce_when_started(
            asyncio.get_running_loop(),
            lambda: False,
            said.append,
            9100,
            poll_seconds=0.001,
        )
        await asyncio.sleep(0.05)

    asyncio.run(attempt())
    assert said == []
