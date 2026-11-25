# SPDX-License-Identifier: MIT
"""Launch the BNO085 GUI: `python3 -m pico_gui_bno085`."""

import argparse
import functools
import sys
import time
from collections.abc import Sequence
from pathlib import Path

from pico_gui import launch
from pico_gui.device import open_device
from pico_gui.recorder import Recorder
from pico_gui.replay import ReplayTransport
from pico_gui.session import Session
from pico_gui.transport import Transport, TransportError
from pico_gui_bno085 import panel
from pico_gui_bno085.monitor import Monitor

REPLAY_LINES_PER_SECOND = 2000.0
EXIT_CANNOT_RUN = 2


def parser() -> argparse.ArgumentParser:
    """Return the launcher's argument parser."""
    result = argparse.ArgumentParser(
        prog="python3 -m pico_gui_bno085",
        description="Show the BNO085 bench stream in a browser.",
    )
    source = result.add_mutually_exclusive_group(required=True)
    source.add_argument("--serial", help="serial number of the board to open")
    source.add_argument("--replay", type=Path, help="recording to replay")
    result.add_argument(
        "--device-port",
        help="serial port of the board, if it has more than one",
    )
    result.add_argument("--record", type=Path, help="write received lines to a file")
    result.add_argument(
        "--replay-rate",
        type=float,
        default=REPLAY_LINES_PER_SECOND,
        help="lines a second of a replay (default %(default)s)",
    )
    launch.add_server_arguments(result)
    return result


def open_source(args: argparse.Namespace) -> tuple[str, Transport]:
    """Return the data source's label and transport; failures raise."""
    if args.replay is not None:
        return f"replay {args.replay.name}", ReplayTransport.from_path(
            args.replay,
            lines_per_second=args.replay_rate,
            sleep=time.sleep,
        )
    return f"board {args.serial}", open_device(args.serial, port=args.device_port)


def refuse(error: TransportError) -> int:
    """Report an expected failure on standard error and return its status."""
    print(f"pico_gui_bno085: {error}", file=sys.stderr)
    return EXIT_CANNOT_RUN


def main(argv: Sequence[str] | None = None) -> int:
    """Run the launcher and return its exit status."""
    args = parser().parse_args(argv)
    try:
        label, transport = open_source(args)
    except TransportError as error:
        return refuse(error)
    try:
        recorder = None if args.record is None else Recorder.open(args.record)
    except TransportError as error:
        transport.close()
        return refuse(error)
    monitor = Monitor(time.monotonic)
    session = Session(
        transport,
        monitor.on_line,
        clock=time.monotonic,
        recorder=recorder,
    )
    session.start()
    launch.run(
        "BNO085",
        launch.Server(args.host, args.port),
        functools.partial(panel.build, label, lambda: session.status, monitor.snapshot),
        session.stop,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
