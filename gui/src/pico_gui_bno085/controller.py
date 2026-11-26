# SPDX-License-Identifier: MIT
"""Send bench commands, match each to its reply and keep the stream running.

The controller owns no thread and no clock: lines arrive through `on_line`
from the reader thread, time arrives through `start`, `submit` and `tick`
from the caller, and every send goes through the callable it was built with.
Every expected failure becomes the detail of a `ControllerState`. Only
`submit` raises, `CommandError` for a command it refuses, which the
interface catches and shows.

The bench reads and acts on commands at any time, including while a `run`
is printing, so a queued command goes out as soon as no other command is in
flight. The commands the driver refuses before the session is READY (LP-2)
wait for a `# ready` line first. The one-second run length bounds how long
telemetry keeps flowing after the last sensor is disabled, and how long the
stream stays silent after a lost `# done` line, the run length plus a
two-second grace, before the controller asks again.
"""

import threading
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from enum import Enum

from pico_gui.commands import CommandError, command_bytes
from pico_gui.transport import TransportError
from pico_gui_bno085.commands import (
    DeviceCommand,
    connect_bus,
    connect_payload,
    run,
)

_DEFAULT_RUN_MS = 1000
# The reply wait is a chosen value, not one the specification states: nothing
# in LP-2 or LP-3 says how long a bus takes to come up or a command to be
# answered. A board slower than this is not lost: `retry_connect` asks again
# and a late `# bus` reply is taken as the session coming up.
_REPLY_TIMEOUT_SECONDS = 2.0
_RUN_GRACE_SECONDS = 2.0
# LP-3: the lines a command of this controller can be answered with. Only a
# line of one of these forms can be a late answer to a timed-out command;
# telemetry, the done and ready lines, the stats, info, calibration and
# response lines, the trace lines and the firmware's own log lines are not
# replies and leave a standing suspicion alone.
_REPLY_CANDIDATES = ("# ok", "# err", "# bus ", "# payload ", "# run ")

__all__ = ("Controller", "ControllerState", "Phase")


class Phase(Enum):
    """Where the session's command path stands."""

    CONNECTING = "connecting"
    LIVE = "live"
    REFUSED = "refused"


@dataclass(frozen=True)
class ControllerState:
    """What the interface reads of the controller at one instant."""

    phase: Phase
    ready: bool
    streaming: bool
    enabled: frozenset[int]
    commanded: Mapping[int, int]
    detail: str
    in_flight: str | None
    queued: int


class Controller:
    """The session's command path over an open transport.

    `send` receives the bytes of a validated command line. `on_line` takes
    every line the session reads, replies and telemetry alike, with the
    caller's clock reading. `tick` takes a clock reading whenever the caller
    wants the timeouts checked.

    `_ready` is never cleared once set. No line of the protocol announces
    that the hub has left READY: a `# ready` line only ever adds it, so a
    calibration command sent while the hub is restarting is answered
    `# err not_ready` (LP-3), which shows in the detail, and the next
    `# ready` line makes the wait worth trying again.

    A command whose reply does not come within its wait is kept on a list
    of suspects, oldest first, because the bench answers every command in
    order and a late answer can only sit ahead of the reply to the command
    sent after it. A line that can be a reply — one beginning `# ok`,
    `# err`, `# bus`, `# payload` or `# run` — is tested against the oldest
    suspect: if it has that command's reply form it is dropped and the
    suspect is removed, while the command in flight keeps its turn for its
    own reply; if it does not, the oldest suspect's reply was lost, the
    suspect is removed and the line is tested against the next, until
    either one matches or the list is empty and the line serves the command
    in flight. A command whose reply was a dropped one is not added to the
    list when its own wait then passes: its outcome is unknown, and the
    detail says so. That reading has a limit the order of replies cannot
    resolve: when a suspect reply is dropped while a command is in
    flight, the dropped line may have been that command's own reply
    rather than the earlier suspect's late answer, and the controller
    reads it as the suspect's answer. If every reply was only slow, the
    marked command's own late reply then arrives after its wait has
    passed and is applied to the command sent after it; two consecutive
    slow replies bring that about. Command A times out, its late reply is
    dropped as a suspect's and marks B, B times out as unknown, and B's own
    late reply is then applied to the command C sent after it. Each suspect
    leaves once its wait has passed by more than one further reply wait —
    two waits past its own, because the command sent after a timeout
    answers at least one wait later — and a retry of the connect sequence
    clears the list.
    """

    def __init__(
        self,
        send: Callable[[bytes], None],
        bus: str,
        *,
        run_ms: int = _DEFAULT_RUN_MS,
        reply_timeout: float = _REPLY_TIMEOUT_SECONDS,
        run_grace: float = _RUN_GRACE_SECONDS,
    ) -> None:
        self._send = send
        self._run_ms = run_ms
        self._reply_timeout = reply_timeout
        self._run_grace = run_grace
        self._bus_command = connect_bus(bus)
        self._payload_command = connect_payload()
        self._lock = threading.Lock()
        self._queue: list[DeviceCommand] = []
        self._in_flight: DeviceCommand | None = None
        self._in_flight_sent_at = 0.0
        self._suspects: list[tuple[DeviceCommand, float]] = []
        self._reply_dropped_for: DeviceCommand | None = None
        self._phase = Phase.CONNECTING
        self._ready = False
        self._streaming = False
        self._run_sent_at = 0.0
        self._enabled: set[int] = set()
        self._commanded: dict[int, int] = {}
        self._detail = ""

    def start(self, now: float) -> None:
        """Begin the session: select the bus, then turn the payload on."""
        with self._lock:
            self._queue.append(self._bus_command)
            self._queue.append(self._payload_command)
        self._pump(now)

    def submit(self, command: DeviceCommand, now: float) -> None:
        """Queue a command from the interface.

        A line the protocol refuses raises `CommandError` before anything is
        queued. On a session whose bus never came up it raises as well,
        because the bench answers every such command with an error naming
        the missing bus.
        """
        command_bytes(command.line)
        with self._lock:
            if self._phase is Phase.REFUSED:
                raise CommandError(
                    "the bus never came up, so the bench takes no command",
                )
            self._queue.append(command)
        self._pump(now)

    def retry_connect(self, now: float) -> None:
        """Ask for the bus again after a refusal, a timeout or a send failure.

        The bench re-runs the whole connect sequence: it takes `bus` at any
        time. That it then re-enables the sensors it still holds as desired
        is UNCONFIRMED: the specification states the re-enabling for a hub
        reset (AR-3) and the firmware code carries a desired table through
        `begin()`, but no device run has shown it across a bus retry, so the
        enabled set and the commanded intervals are kept as the session's
        best knowledge.
        """
        with self._lock:
            if self._phase is not Phase.REFUSED:
                return
            self._phase = Phase.CONNECTING
            self._detail = ""
            self._suspects.clear()
            self._reply_dropped_for = None
            self._queue.append(self._bus_command)
            self._queue.append(self._payload_command)
        self._pump(now)

    def on_line(self, text: str, now: float) -> None:
        """Take one line of the stream and match it to the command in flight."""
        with self._lock:
            self._dispatch(text)
        self._pump(now)

    def tick(self, now: float) -> None:
        """Check for a missing reply and for a run that outlived its window."""
        with self._lock:
            in_flight = self._in_flight
            if (
                in_flight is not None
                and now - self._in_flight_sent_at > self._reply_timeout
            ):
                if in_flight is self._reply_dropped_for:
                    # A reply that may have answered this command was
                    # already dropped as late for an earlier one, so its
                    # own reply may never come: the outcome is unknown and
                    # the command is not added to the suspects.
                    self._detail = (
                        f"the outcome of '{in_flight.line}' is unknown: its "
                        "reply may be the one already dropped"
                    )
                    self._timeout(in_flight)
                    self._reply_dropped_for = None
                else:
                    self._detail = (
                        f"no reply to '{in_flight.line}' within "
                        f"{self._reply_timeout:g} s"
                    )
                    self._timeout(in_flight)
                    # The reply may still arrive and be taken for the answer
                    # to a later command, so the command joins the suspects.
                    self._suspects.append((in_flight, now))
                if in_flight.run:
                    # The bench may never have seen the run, so nothing may
                    # answer it with a done line; ask again.
                    self._keep_running()
            # A late answer is ordered ahead of the reply to any command sent
            # after it, and the command sent after a timeout has its own
            # wait pass one reply wait later still, so an entry is kept for
            # more than one further reply wait: two waits past its own. A
            # shorter bound would remove every earlier entry at the tick
            # that stamps the next one.
            self._suspects = [
                entry
                for entry in self._suspects
                if now - entry[1] <= 2 * self._reply_timeout
            ]
            if (
                self._streaming
                and now - self._run_sent_at > self._run_ms / 1000 + self._run_grace
            ):
                self._detail = "the run ended without its done line; restarted"
                self._streaming = False
                self._keep_running()
        self._pump(now)

    def state(self) -> ControllerState:
        """Return the state at one instant."""
        with self._lock:
            return ControllerState(
                self._phase,
                self._ready,
                self._streaming,
                frozenset(self._enabled),
                dict(self._commanded),
                self._detail,
                None if self._in_flight is None else self._in_flight.line,
                len(self._queue),
            )

    def _dispatch(self, text: str) -> None:
        """Change state for one line; the caller sends what the queue allows.

        Runs under the caller's lock and never writes to the stream itself.
        """
        if text == "# ready":
            self._ready = True
        if text.startswith("# done"):
            self._streaming = False
            self._keep_running()
        if (
            text.startswith("# bus ")
            and self._in_flight is None
            and self._phase is Phase.REFUSED
        ):
            # The bus answer arrived after its wait had passed: the bus is up,
            # so the connect sequence continues rather than waiting for a
            # retry.
            self._detail = f"the bus answer arrived late: {text}"
            self._suspects.clear()
            self._phase = Phase.CONNECTING
            self._queue.append(self._payload_command)
            return
        in_flight = self._in_flight
        if (
            text.startswith(_REPLY_CANDIDATES)
            and self._suspects
            and self._screen_suspects(text, in_flight)
        ):
            return
        if in_flight is None:
            return
        if text.startswith("# err"):
            self._detail = f"'{in_flight.line}' was refused: {text}"
            self._in_flight = None
            self._refused(in_flight)
            return
        if text.startswith(in_flight.success):
            self._accept(in_flight)
            self._in_flight = None

    def _screen_suspects(
        self,
        text: str,
        in_flight: DeviceCommand | None,
    ) -> bool:
        """Test a reply candidate against the suspects, oldest first.

        The bench answers commands in order, so the oldest suspect's late
        answer, if it comes, comes first. Returns True when the line was
        dropped for a suspect; a line that answers no suspect empties the
        list on its way through — each unmatched entry's reply was lost —
        and is then applied to the command in flight by `_dispatch`.
        """
        while self._suspects:
            oldest = self._suspects[0][0]
            if text.startswith(("# err", oldest.success)):
                # The line may be the oldest suspect's late answer: it is
                # dropped, the suspect leaves the list, and the command in
                # flight keeps its turn for its own reply.
                if in_flight is None:
                    self._detail = (
                        f"the reply answered '{oldest.line}' after its wait had passed"
                    )
                else:
                    self._detail = (
                        f"the reply may answer '{oldest.line}', "
                        f"so '{in_flight.line}' still awaits its own"
                    )
                    self._reply_dropped_for = in_flight
                self._suspects.pop(0)
                return True
            # The line cannot answer the oldest suspect, so that suspect's
            # reply was lost: it leaves the list and the line is tested
            # against the next.
            self._suspects.pop(0)
        return False

    def _refused(self, command: DeviceCommand) -> None:
        """Note what a refusal leaves behind.

        A refused bus ends the session's command path, because every other
        command would be answered with the missing-bus error. A refused
        payload still leaves the session live: the bench answers every other
        command and prints its E lines (LP-3).
        """
        if command is self._bus_command:
            self._phase = Phase.REFUSED
            self._queue.clear()
        elif command is self._payload_command:
            self._phase = Phase.LIVE

    def _timeout(self, command: DeviceCommand) -> None:
        """Note what a missing reply leaves behind.

        The bus that never answered never came up; the payload that never
        answered leaves a working bus, so the session goes live with the
        timeout as its detail.
        """
        self._in_flight = None
        if command is self._bus_command:
            self._phase = Phase.REFUSED
            self._queue.clear()
        elif command is self._payload_command:
            self._phase = Phase.LIVE

    def _accept(self, command: DeviceCommand) -> None:
        if command is self._payload_command:
            self._phase = Phase.LIVE
        if command.sensor is not None:
            if command.enables:
                self._enabled.add(command.sensor)
                if command.interval_us is not None:
                    self._commanded[command.sensor] = command.interval_us
                self._keep_running()
            else:
                self._enabled.discard(command.sensor)
                self._commanded.pop(command.sensor, None)
        if command.run:
            self._streaming = True
            self._run_sent_at = self._in_flight_sent_at

    def _keep_running(self) -> None:
        """Queue a run while any sensor is enabled, once and only once."""
        if not self._enabled:
            return
        if self._streaming:
            return
        if self._in_flight is not None and self._in_flight.run:
            return
        if any(queued.run for queued in self._queue):
            return
        self._queue.append(run(self._run_ms))

    def _pump(self, now: float) -> None:
        """Send the first queued command the bench will act on, one at a time.

        The command's slot is reserved under the lock, so two callers cannot
        both send, and the write runs outside the lock, so a stalled stream
        never blocks `state`.
        """
        with self._lock:
            data = self._reserve_next(now)
        if data is None:
            return
        try:
            self._send(data)
        except TransportError as error:
            with self._lock:
                command = self._in_flight
                line = "" if command is None else command.line
                self._in_flight = None
                # The stream is gone or stalled; the reader reports it in
                # the session status, and the interface can retry the
                # connect sequence once the stream answers again.
                self._detail = f"cannot send '{line}': {error}"
                self._phase = Phase.REFUSED
                self._queue.clear()

    def _reserve_next(self, now: float) -> bytes | None:
        """Reserve the next actionable command and return its bytes.

        Runs under the caller's lock. A command held for READY, a refused
        session and a command already in flight stop the reservation.
        """
        while (
            self._in_flight is None and self._queue and self._phase is not Phase.REFUSED
        ):
            command = self._queue[0]
            if command.needs_ready and not self._ready:
                self._detail = f"waiting for the hub to be ready for '{command.line}'"
                return None
            self._queue.pop(0)
            try:
                data = command_bytes(command.line)
            except CommandError as error:
                self._detail = str(error)
                continue
            self._in_flight = command
            self._in_flight_sent_at = now
            return data
        return None
