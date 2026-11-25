# SPDX-License-Identifier: MIT
"""Start a GUI server and tell the user how to reach it."""

import argparse
import contextlib
from collections.abc import Callable
from dataclasses import dataclass

from nicegui import app, ui

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8080
MAX_PORT = 65535


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
    if not 1 <= port <= MAX_PORT:
        raise argparse.ArgumentTypeError(f"{port} is not between 1 and {MAX_PORT}")
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

    The first step follows the Dev Containers documentation. VS Code's
    documentation does not describe removing a forwarded port, so the last
    step is UNCONFIRMED.
    """
    return (
        f"To open the GUI in the browser on the host, forward port {port} in VS Code:\n"
        f"  1. Press F1 and run 'Forward a Port', or open the Ports view and add a\n"
        f"     port, and enter {port}.\n"
        f"  2. Open the forwarded address that VS Code shows; its local port can\n"
        f"     differ from {port}.\n"
        f"To stop forwarding, remove port {port} in the Ports view\n"
        f"(UNCONFIRMED: the VS Code documentation does not describe this step).\n"
        f"Press Ctrl+C here to stop the server."
    )


def shutdown_message(port: int) -> str:
    """Return what the launcher says when the server has stopped."""
    return (
        f"Server stopped. Forwarded port {port} can now be removed in the Ports view."
    )


def run(
    title: str,
    server: Server,
    build_page: Callable[[], None],
    on_shutdown: Callable[[], None],
) -> None:
    """Serve `build_page` at `/` until interrupted, then run `on_shutdown`.

    Output goes to standard output. Ctrl+C stops the server cleanly.
    """
    print(forwarding_steps(server.port), flush=True)
    ui.page("/")(build_page)
    app.on_shutdown(on_shutdown)
    with contextlib.suppress(KeyboardInterrupt):
        ui.run(
            host=server.host,
            port=server.port,
            title=title,
            reload=False,
            show=False,
        )
    print(shutdown_message(server.port), flush=True)
