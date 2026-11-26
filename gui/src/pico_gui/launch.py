# SPDX-License-Identifier: MIT
"""Start a GUI server and tell the user how to reach it."""

import argparse
import asyncio
import contextlib
import sys
from collections.abc import Callable
from dataclasses import dataclass

from nicegui import app, ui
from nicegui import server as nicegui_server

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8080
_MAX_PORT = 65535
_POLL_SECONDS = 0.05

__all__ = (
    "DEFAULT_HOST",
    "DEFAULT_PORT",
    "Server",
    "add_server_arguments",
    "announce_when_started",
    "bind_failure_message",
    "forwarding_steps",
    "port_number",
    "run",
    "shutdown_message",
)


@dataclass(frozen=True)
class Server:
    """Where the GUI server listens."""

    host: str
    port: int


def port_number(text: str) -> int:
    """Parse a TCP port for `argparse`."""
    try:
        port = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"{text!r} is not a port number") from None
    if not 1 <= port <= _MAX_PORT:
        raise argparse.ArgumentTypeError(f"{port} is not between 1 and {_MAX_PORT}")
    return port


def add_server_arguments(parser: argparse.ArgumentParser) -> None:
    """Add `--host` and `--port` to a launcher's parser."""
    parser.add_argument(
        "--host",
        default=DEFAULT_HOST,
        help="address to listen on (default %(default)s)",
    )
    parser.add_argument(
        "--port",
        type=port_number,
        default=DEFAULT_PORT,
        help="port of the GUI server (default %(default)s)",
    )


def forwarding_steps(port: int) -> str:
    """Return the steps that let the Windows browser reach `port`.

    Source: the Dev Containers documentation, "Forwarding or publishing a
    port". It names the notification that follows the Forward a Port
    command, gives `http://localhost:4123` as the example of the address
    to open, and states that a temporary forward lasts for the duration
    of the session. It names no default local port, so the text does
    not. It does not describe stopping a forward before the VS Code
    session ends, so the text does not.
    """
    return (
        f"The GUI server is listening on port {port}. To open it in the browser\n"
        f"on the host, forward the port in VS Code:\n"
        f"  1. Run 'Forward a Port' from the Command Palette (F1) and enter\n"
        f"     {port}. A notification will tell you the localhost port you\n"
        f"     should use.\n"
        f"  2. Open the localhost port the notification names in the browser,\n"
        f"     for example http://localhost:4123.\n"
        f"A temporary forward lasts for the duration of the VS Code session.\n"
        f"Press Ctrl+C here to stop the server."
    )


def shutdown_message() -> str:
    """Return what the launcher says when the server has stopped."""
    return "Server stopped."


def bind_failure_message(server: Server) -> str:
    """Return what the launcher says when the server could not start."""
    return (
        f"cannot serve on {server.host}:{server.port}; the port may be in use. "
        f"Choose another with --port."
    )


def announce_when_started(
    loop: asyncio.AbstractEventLoop,
    started: Callable[[], bool],
    say: Callable[[str], None],
    port: int,
    *,
    poll_seconds: float = _POLL_SECONDS,
) -> None:
    """Say how to reach `port` once `started` is true, checking on `loop`.

    A server that never binds never says anything.
    """
    if started():
        say(forwarding_steps(port))
        return
    loop.call_later(
        poll_seconds,
        announce_when_started,
        loop,
        started,
        say,
        port,
    )


def run(
    title: str,
    server: Server,
    build_page: Callable[[], object],
    on_shutdown: Callable[[], None],
) -> None:
    """Serve `build_page` at `/` until interrupted, then run `on_shutdown`.

    The page builder runs once per client and its return value is ignored.
    The forwarding steps go to standard output once the server has bound its
    port. Ctrl+C stops the server cleanly. A port that cannot be bound is
    reported on standard error and the exit status is non-zero.
    """

    def say(text: str) -> None:
        print(text, flush=True)

    def announce() -> None:
        announce_when_started(
            asyncio.get_running_loop(),
            lambda: nicegui_server.Server.instance.started,
            say,
            server.port,
        )

    ui.page("/")(build_page)
    app.on_shutdown(on_shutdown)
    app.on_startup(announce)
    try:
        with contextlib.suppress(KeyboardInterrupt):
            ui.run(
                host=server.host,
                port=server.port,
                title=title,
                reload=False,
                show=False,
            )
    except SystemExit:
        if not nicegui_server.Server.instance.started:
            print(bind_failure_message(server), file=sys.stderr, flush=True)
        raise
    print(shutdown_message(), flush=True)
