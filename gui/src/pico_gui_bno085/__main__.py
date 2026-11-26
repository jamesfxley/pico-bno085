# SPDX-License-Identifier: MIT
"""Launch the BNO085 GUI: `python3 -m pico_gui_bno085`."""

import argparse
import functools
import math
import sys
import time
from collections.abc import Sequence
from pathlib import Path

from pico_gui import launch
from pico_gui.device import open_device
from pico_gui.recorder import Recorder, RecorderSlot
from pico_gui.replay import ReplayTransport
from pico_gui.session import Session, State, Status
from pico_gui.transport import Transport, TransportError
from pico_gui_bno085 import panel
from pico_gui_bno085.commands import DeviceCommand
from pico_gui_bno085.controller import Controller
from pico_gui_bno085.model import Snapshot
from pico_gui_bno085.monitor import Monitor
from pico_gui_bno085.panel import (
    CommandSurface,
    RecordingSurface,
    ReplaySurface,
    Sources,
)
from pico_gui_bno085.protocol import Telemetry

_REPLAY_LINES_PER_SECOND = 2000.0
_EXIT_CANNOT_RUN = 2
_BUSES = ("i2c", "spi")

__all__ = (
    "BoardRuntime",
    "RecorderControl",
    "ReplayRuntime",
    "close_recordings",
    "main",
    "parser",
    "refuse",
)


def _positive_number(text: str) -> float:
    """Parse a finite number above zero for `argparse`."""
    try:
        number = float(text)
    except ValueError:
        raise argparse.ArgumentTypeError(f"{text!r} is not a number") from None
    if not math.isfinite(number) or number <= 0:
        raise argparse.ArgumentTypeError(f"{text!r} is not above zero")
    return number


def parser() -> argparse.ArgumentParser:
    """Return the launcher's argument parser."""
    result = argparse.ArgumentParser(
        prog="python3 -m pico_gui_bno085",
        description="Show and drive the BNO085 bench stream in a browser.",
    )
    source = result.add_mutually_exclusive_group(required=True)
    source.add_argument("--serial", help="serial number of the board to open")
    source.add_argument("--replay", type=Path, help="recording to replay")
    result.add_argument(
        "--bus",
        choices=_BUSES,
        help="bus the hub sits on; required with --serial",
    )
    result.add_argument(
        "--device-port",
        help="serial port of the board, if it has more than one",
    )
    result.add_argument(
        "--record",
        type=Path,
        help="write received lines to a new file; an existing file is refused",
    )
    result.add_argument(
        "--record-dir",
        type=Path,
        help="directory the record button writes generated files into",
    )
    result.add_argument(
        "--replay-rate",
        type=_positive_number,
        default=_REPLAY_LINES_PER_SECOND,
        help="lines a second of a replay (default %(default)s)",
    )
    launch.add_server_arguments(result)
    return result


def refuse(error: TransportError) -> int:
    """Report an expected failure on standard error and return its status."""
    print(f"pico_gui_bno085: {error}", file=sys.stderr)
    return _EXIT_CANNOT_RUN


class RecorderControl:
    """The record button behind a `--record-dir` directory.

    The control owns the slot the session writes through. `start` generates
    the file name, so no name is typed into the browser; it refuses, with a
    note the panel shows, while a recording is active. A write failure
    detaches the recorder on the reader thread and shows here through
    `note_failure`, without ending the session.
    """

    def __init__(self, directory: Path) -> None:
        self._directory = directory
        self._slot = RecorderSlot(on_failure=self.note_failure)
        self._name = ""
        self._message = ""

    @property
    def slot(self) -> RecorderSlot:
        """Return the slot the session writes this control's lines through."""
        return self._slot

    def start(self) -> None:
        """Open a generated file in the directory and record into it."""
        if self._slot.attached:
            self._message = "a recording is already active; stop it first"
            return
        self._message = ""
        stem = f"session-{time.strftime('%Y%m%d-%H%M%S', time.gmtime(time.time()))}"
        name = f"{stem}.txt"
        suffix = 0
        while (self._directory / name).exists():
            suffix += 1
            name = f"{stem}-{suffix}.txt"
        recorder = Recorder.open(self._directory / name)
        self._slot.attach(recorder)
        self._name = name

    def stop(self) -> None:
        """Stop writing and close the recording."""
        self._message = ""
        recorder = self._slot.detach()
        if recorder is not None:
            recorder.close()

    def note_failure(self, detail: str) -> None:
        """Record a write failure reported from the reader thread."""
        self._message = f"the recording failed: {detail}"
        self._name = ""

    def surface(self) -> RecordingSurface:
        """Return the interface the page drives this control through."""
        return RecordingSurface(
            start=self.start,
            stop=self.stop,
            active=lambda: self._slot.attached,
            name=lambda: self._name,
            failure=lambda: self._message,
        )


class ReplayRuntime:
    """The replay source: its session, and the controls that hold it.

    A restart stops the current pass and starts the recording again over a
    fresh transport and a fresh model, so the page shows the recording from
    its start. A pass's end leaves the recorder slot attached, so a
    recording the interface started keeps writing through the next pass until
    it is stopped or the server shuts down.
    """

    def __init__(
        self,
        path: Path,
        *,
        lines_per_second: float | None,
        recorder: RecorderSlot | None,
    ) -> None:
        self._path = path
        self._lines_per_second = lines_per_second
        self._recorder = recorder
        self._session: Session | None = None
        self._transport: ReplayTransport | None = None
        self._monitor = Monitor()

    def start(self) -> None:
        """Open the recording and read it on a new session."""
        transport = ReplayTransport.from_path(
            self._path,
            lines_per_second=self._lines_per_second,
            sleep=time.sleep,
        )
        self._transport = transport
        self._monitor = Monitor()
        self._session = Session(
            transport,
            self._monitor.on_line,
            clock=time.monotonic,
            recorder=self._recorder,
        )
        self._session.start()

    def wait(self, timeout: float | None = None) -> None:
        """Wait for the current pass's reader to finish."""
        if self._session is not None:
            self._session.wait(timeout)

    def restart(self) -> None:
        """Stop the current pass and start the recording again."""
        self.shutdown()
        self.start()

    def pause(self) -> None:
        """Hold the pass where it stands; the session stays open."""
        if self._transport is not None:
            self._transport.pause()

    def resume(self) -> None:
        """Continue a held pass."""
        if self._transport is not None:
            self._transport.resume()

    def progress(self) -> float:
        """Return the fraction of the recording the pass has read."""
        if self._transport is None:
            return 1.0
        return self._transport.progress()

    def paused(self) -> bool:
        """Return whether the pass is held."""
        return self._transport is not None and self._transport.is_paused()

    def status(self) -> Status:
        """Return the current pass's session status."""
        if self._session is None:
            return Status(State.ENDED)
        return self._session.status

    def snapshot(self) -> Snapshot:
        """Return the current pass's model state."""
        return self._monitor.snapshot()

    def history(self, sensor: int) -> tuple[Telemetry, ...]:
        """Return the samples the current pass retains of `sensor`."""
        return self._monitor.history(sensor)

    def surface(self) -> ReplaySurface:
        """Return the interface the page drives this runtime through."""
        return ReplaySurface(
            pause=self.pause,
            resume=self.resume,
            restart=self.restart,
            progress=self.progress,
            paused=self.paused,
        )

    def shutdown(self) -> None:
        """Stop the pass and release its transport."""
        if self._session is not None:
            self._session.stop()
            self._session = None
        if self._transport is not None:
            self._transport.close()
            self._transport = None


class BoardRuntime:
    """The board source: its session, controller and monitor.

    The controller sends through the transport from whichever thread calls
    it; the monitor parses what the reader thread reads. Every line goes to
    both.
    """

    def __init__(
        self,
        transport: Transport,
        bus: str,
        recorder: RecorderSlot | None,
    ) -> None:
        self._controller = Controller(transport.send, bus)
        self._monitor = Monitor()

        def on_line(text: str, now: float) -> None:
            self._controller.on_line(text, now)
            self._monitor.on_line(text, now)

        self._session = Session(
            transport,
            on_line,
            clock=time.monotonic,
            recorder=recorder,
        )
        # The controller's first command must be in flight before the reader
        # starts, so the connect replies cannot pass their matchers.
        self._controller.start(time.monotonic())
        self._session.start()

    def status(self) -> Status:
        return self._session.status

    def snapshot(self) -> Snapshot:
        return self._monitor.snapshot(self._controller.state().commanded)

    def history(self, sensor: int) -> tuple[Telemetry, ...]:
        """Return the samples the model retains of `sensor`."""
        return self._monitor.history(sensor)

    def surface(self) -> CommandSurface:
        def submit(command: DeviceCommand) -> None:
            self._controller.submit(command, time.monotonic())

        def retry() -> None:
            self._controller.retry_connect(time.monotonic())

        return CommandSurface(
            submit=submit,
            state=self._controller.state,
            retry=retry,
        )

    def tick(self) -> None:
        """Let the controller check its timeouts against the clock."""
        self._controller.tick(time.monotonic())

    def shutdown(self) -> None:
        self._session.stop()


def main(argv: Sequence[str] | None = None) -> int:
    """Run the launcher and return its exit status."""
    command = parser()
    args = command.parse_args(argv)
    if args.replay is not None:
        if args.bus is not None:
            command.error("--bus selects a bus on a board, not a replay")
        if args.device_port is not None:
            command.error("--device-port applies to --serial, not to --replay")
    elif args.bus is None:
        command.error("--bus is required with --serial")
    if args.record is not None and args.record_dir is not None:
        command.error(
            "--record and --record-dir are two ways to write the same lines",
        )
    if args.record is not None and args.record.exists():
        return refuse(
            TransportError(
                f"--record {args.record} names a file that exists; "
                "choose a new name, since a recording is never overwritten",
            ),
        )
    try:
        if args.replay is not None:
            return _serve_replay(args)
        return _serve_board(args)
    except TransportError as error:
        return refuse(error)


def close_recordings(*slots: RecorderSlot | None) -> None:
    """Close every attached recorder; a failure is said and swallowed.

    The shutdown hook runs once the server has stopped. Each line of a
    recording is already on disk, so a close that fails costs nothing but
    the notice on standard error.
    """
    for slot in slots:
        if slot is None:
            continue
        recorder = slot.detach()
        if recorder is None:
            continue
        try:
            recorder.close()
        except TransportError as error:
            print(f"pico_gui_bno085: {error}", file=sys.stderr)


def _serve_board(args: argparse.Namespace) -> int:
    """Serve a page over a board and return the exit status."""
    transport = open_device(args.serial, port=args.device_port)
    try:
        recorder = (
            None
            if args.record is None
            else RecorderSlot(
                Recorder.open(args.record),
            )
        )
        control = (
            None
            if args.record_dir is None
            else RecorderControl(
                args.record_dir,
            )
        )
    except TransportError:
        transport.close()
        raise
    slot = (
        recorder
        if recorder is not None
        else (None if control is None else control.slot)
    )
    runtime = BoardRuntime(transport, args.bus, slot)
    recording = None if control is None else control.surface()

    def shutdown() -> None:
        runtime.shutdown()
        close_recordings(recorder, None if control is None else control.slot)

    sources = Sources(
        label=f"board {args.serial}",
        status=runtime.status,
        snapshot=runtime.snapshot,
        history=runtime.history,
        tick=runtime.tick,
        commands=runtime.surface(),
        replay=None,
        recording=recording,
    )
    launch.run(
        "BNO085",
        launch.Server(args.host, args.port),
        functools.partial(panel.build_page, sources),
        shutdown,
    )
    return 0


def _serve_replay(args: argparse.Namespace) -> int:
    """Serve a page over a recording and return the exit status."""
    recorder = (
        None
        if args.record is None
        else RecorderSlot(
            Recorder.open(args.record),
        )
    )
    control = (
        None
        if args.record_dir is None
        else RecorderControl(
            args.record_dir,
        )
    )
    slot = (
        recorder
        if recorder is not None
        else (None if control is None else control.slot)
    )
    runtime = ReplayRuntime(
        args.replay,
        lines_per_second=args.replay_rate,
        recorder=slot,
    )
    try:
        runtime.start()
    except TransportError:
        # A recording that cannot be opened leaves no half-written record.
        detached = None if recorder is None else recorder.detach()
        if detached is not None:
            detached.close()
        raise
    recording = None if control is None else control.surface()

    def shutdown() -> None:
        runtime.shutdown()
        close_recordings(recorder, None if control is None else control.slot)

    sources = Sources(
        label=f"replay {args.replay.name}",
        status=runtime.status,
        snapshot=runtime.snapshot,
        history=runtime.history,
        tick=lambda: None,
        commands=None,
        replay=runtime.surface(),
        recording=recording,
    )
    launch.run(
        "BNO085",
        launch.Server(args.host, args.port),
        functools.partial(panel.build_page, sources),
        shutdown,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
