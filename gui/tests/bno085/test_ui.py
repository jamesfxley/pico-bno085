# SPDX-License-Identifier: MIT
"""The page under NiceGUI's headless user simulation."""

from collections.abc import Iterable
from pathlib import Path
from typing import cast

import pytest
from nicegui import ui
from nicegui.testing import User

from pico_gui.session import State, Status
from pico_gui.shell import REFRESH_INTERVAL_SECONDS
from pico_gui_bno085 import panel
from pico_gui_bno085.commands import DeviceCommand, cal, calget, cfg, off, tare
from pico_gui_bno085.controller import ControllerState, Phase
from pico_gui_bno085.monitor import Monitor
from pico_gui_bno085.panel import (
    NO_BOARD,
    CommandSurface,
    RecordingSurface,
    ReplaySurface,
    Sources,
    missing_rotation_text,
    motion_window_text,
    no_motion_text,
    reference_text,
    rotation_options,
    summary,
)

DATA = Path(__file__).resolve().parent.parent / "data"

# One sample of each rotation kind (LP-6), as a board prints them.
ROTATION_LINES = (
    "# run 3000",
    "Q,5,0,1000000,3,0.24121,-0.03625,0.03137,-0.96930,3.14160",
    "R,8,0,1010000,3,0.99884,-0.03918,-0.02765,0.00110",
    "G,42,0,1020000,0,0.99884,-0.03918,-0.02765,0.00110,0.00879,-0.00293,-0.00488",
)


def replayed() -> Monitor:
    """Return a monitor that has read the recording."""
    monitor = Monitor()
    for line in (DATA / "session.txt").read_text(encoding="ascii").splitlines():
        monitor.on_line(line, 0.0)
    return monitor


def fed(lines: Iterable[str]) -> Monitor:
    """Return a monitor that has read `lines`."""
    monitor = Monitor()
    for line in lines:
        monitor.on_line(line, 0.0)
    return monitor


class FakeCommands:
    """A command surface that records what the page submits and retries."""

    def __init__(self) -> None:
        self.submitted: list[DeviceCommand] = []
        self.retries = 0
        self.phase = Phase.LIVE

    def submit(self, command: DeviceCommand) -> None:
        self.submitted.append(command)

    def retry(self) -> None:
        self.retries += 1

    def state(self) -> ControllerState:
        return ControllerState(
            self.phase,
            True,
            False,
            frozenset({0x05}),
            {0x05: 10_000},
            "",
            None,
            0,
        )

    def surface(self) -> CommandSurface:
        return CommandSurface(
            submit=self.submit,
            state=self.state,
            retry=self.retry,
        )


class FakeReplay:
    """A replay surface that records the holds and restarts."""

    def __init__(self) -> None:
        self._held = False
        self.restarts = 0

    def held(self) -> bool:
        return self._held

    def surface(self) -> ReplaySurface:
        return ReplaySurface(
            pause=self.pause,
            resume=self.resume,
            restart=self.restart,
            progress=lambda: 0.5,
            paused=self.held,
        )

    def pause(self) -> None:
        self._held = True

    def resume(self) -> None:
        self._held = False

    def restart(self) -> None:
        self.restarts += 1


class FakeRecording:
    """A recording surface that records the starts and stops."""

    def __init__(self) -> None:
        self.active = False
        self.calls: list[str] = []

    def surface(self) -> RecordingSurface:
        return RecordingSurface(
            start=self.start,
            stop=self.stop,
            active=lambda: self.active,
            name=lambda: "session-20261125-160000.txt",
            failure=lambda: "",
        )

    def start(self) -> None:
        self.active = True
        self.calls.append("start")

    def stop(self) -> None:
        self.active = False
        self.calls.append("stop")


class FakeClock:
    """A clock a test moves by hand."""

    def __init__(self) -> None:
        self.now = 1000.0

    def __call__(self) -> float:
        return self.now


async def open_page(
    user: User,
    sources: Sources,
    clock: FakeClock | None = None,
) -> panel.Page:
    """Build the page over `sources`, open it and return its handle."""
    pages: list[panel.Page] = []

    @ui.page("/")
    def build() -> None:
        if clock is None:
            pages.append(panel.build_page(sources))
        else:
            pages.append(panel.Page(sources, clock))

    await user.open("/")
    page = pages[0]
    page.refresh()
    return page


async def test_the_page_renders_the_replayed_stream(user: User) -> None:
    monitor = replayed()
    replay = FakeReplay()
    sources = Sources(
        label="replay session.txt",
        status=lambda: Status(State.ENDED, "", 100, 0),
        snapshot=monitor.snapshot,
        history=monitor.history,
        tick=lambda: None,
        commands=None,
        replay=replay.surface(),
        recording=None,
    )
    page = await open_page(user, sources)
    await user.should_see("replay session.txt")
    await user.should_see(NO_BOARD)
    await user.should_see("replay at 50%")
    # The recording carries a `# cal` response, so the panel decodes it.
    await user.should_see("status 0 (0 is success); accelerometer on")
    await user.should_see("last command response: # dcd status=0")
    await user.should_see(summary(monitor.snapshot()))
    assert [row["sensor"] for row in page.sensors.rows] == [
        "Accelerometer (0x01)",
        "Gyroscope (0x02)",
        "Magnetometer (0x03)",
        "Linear acceleration (0x04)",
        "Rotation vector (0x05)",
        "Gravity (0x06)",
        "Uncalibrated gyroscope (0x07)",
        "Game rotation vector (0x08)",
        "Geomagnetic rotation vector (0x09)",
        "Uncalibrated magnetometer (0x0F)",
        "ARVR-stabilised rotation vector (0x28)",
        "ARVR-stabilised game rotation vector (0x29)",
        "Gyro-integrated rotation vector (0x2A)",
    ]
    health = {row["sensor"]: row for row in page.health.rows}
    assert health["Gyro-integrated rotation vector (0x2A)"]["nominal"].endswith(
        "(observed)",
    )


async def test_every_command_control_is_disabled_in_a_replay(user: User) -> None:
    monitor = replayed()
    commands = FakeCommands()
    sources = Sources(
        label="replay session.txt",
        status=lambda: Status(State.CONNECTED),
        snapshot=monitor.snapshot,
        history=monitor.history,
        tick=lambda: None,
        commands=None,
        replay=FakeReplay().surface(),
        recording=None,
    )
    await open_page(user, sources)
    for marker in ("enable", "disable", "tare", "calibrate", "calget"):
        found = user.find(kind=ui.button, marker=marker)
        assert found.elements, marker
        for element in found.elements:
            assert not element.enabled, marker
    user.find(kind=ui.button, marker="tare").click()
    assert commands.submitted == []


async def test_the_tare_control_sends_the_selected_axes_and_basis(user: User) -> None:
    monitor = replayed()
    commands = FakeCommands()
    sources = Sources(
        label="board EXAMPLE-SERIAL",
        status=lambda: Status(State.CONNECTED),
        snapshot=monitor.snapshot,
        history=monitor.history,
        tick=lambda: None,
        commands=commands.surface(),
        replay=None,
        recording=None,
    )
    page = await open_page(user, sources)
    await user.should_see("commands: live")
    page.axes.value = 0x4
    page.basis.value = 2
    user.find(kind=ui.button, marker="tare").click()
    assert commands.submitted == [tare(0x4, 2)]


async def test_the_enable_control_sends_the_sensor_and_interval(user: User) -> None:
    monitor = replayed()
    commands = FakeCommands()
    sources = Sources(
        label="board EXAMPLE-SERIAL",
        status=lambda: Status(State.CONNECTED),
        snapshot=monitor.snapshot,
        history=monitor.history,
        tick=lambda: None,
        commands=commands.surface(),
        replay=None,
        recording=None,
    )
    page = await open_page(user, sources)
    page.sensor_choice.value = 0x05
    page.interval.value = 5_000.0
    user.find(kind=ui.button, marker="enable").click()
    assert commands.submitted == [cfg(0x05, 5_000)]
    user.find(kind=ui.button, marker="disable").click()
    assert commands.submitted == [cfg(0x05, 5_000), off(0x05)]


async def test_a_refused_interval_is_a_note_and_nothing_is_sent(user: User) -> None:
    monitor = replayed()
    commands = FakeCommands()
    sources = Sources(
        label="board EXAMPLE-SERIAL",
        status=lambda: Status(State.CONNECTED),
        snapshot=monitor.snapshot,
        history=monitor.history,
        tick=lambda: None,
        commands=commands.surface(),
        replay=None,
        recording=None,
    )
    page = await open_page(user, sources)
    page.interval.value = 2.5
    user.find(kind=ui.button, marker="enable").click()
    assert commands.submitted == []
    await user.should_see("whole number of microseconds")


async def test_the_calibration_controls_send_the_switches(user: User) -> None:
    monitor = replayed()
    commands = FakeCommands()
    sources = Sources(
        label="board EXAMPLE-SERIAL",
        status=lambda: Status(State.CONNECTED),
        snapshot=monitor.snapshot,
        history=monitor.history,
        tick=lambda: None,
        commands=commands.surface(),
        replay=None,
        recording=None,
    )
    await open_page(user, sources)
    user.find(kind=ui.switch, marker="mag").click()
    user.find(kind=ui.button, marker="calibrate").click()
    assert commands.submitted == [cal(True, True, False)]
    user.find(kind=ui.button, marker="calget").click()
    assert commands.submitted == [cal(True, True, False), calget()]


async def test_the_replay_controls_hold_and_restart(user: User) -> None:
    monitor = replayed()
    replay = FakeReplay()
    recording = FakeRecording()
    sources = Sources(
        label="replay session.txt",
        status=lambda: Status(State.CONNECTED),
        snapshot=monitor.snapshot,
        history=monitor.history,
        tick=lambda: None,
        commands=None,
        replay=replay.surface(),
        recording=recording.surface(),
    )
    page = await open_page(user, sources)
    user.find(kind=ui.button, marker="hold").click()
    assert replay.held()
    page.refresh()
    await user.should_see("replay at 50%, held")
    user.find(kind=ui.button, marker="hold").click()
    assert not replay.held()
    user.find(kind=ui.button, marker="restart").click()
    assert replay.restarts == 1


async def test_the_recording_controls_start_and_stop(user: User) -> None:
    monitor = replayed()
    recording = FakeRecording()
    sources = Sources(
        label="replay session.txt",
        status=lambda: Status(State.CONNECTED),
        snapshot=monitor.snapshot,
        history=monitor.history,
        tick=lambda: None,
        commands=None,
        replay=FakeReplay().surface(),
        recording=recording.surface(),
    )
    page = await open_page(user, sources)
    user.find(kind=ui.button, marker="record-start").click()
    assert recording.calls == ["start"]
    page.refresh()
    await user.should_see("recording to session-20261125-160000.txt")
    # A recording that runs has its Start control disabled, so a second
    # recording cannot be started over it.
    start = next(iter(user.find(kind=ui.button, marker="record-start").elements))
    assert not start.enabled
    user.find(kind=ui.button, marker="record-stop").click()
    assert recording.calls == ["start", "stop"]
    page.refresh()
    assert start.enabled


async def test_the_retry_control_is_offered_only_while_refused(user: User) -> None:
    monitor = replayed()
    commands = FakeCommands()
    sources = Sources(
        label="board EXAMPLE-SERIAL",
        status=lambda: Status(State.CONNECTED),
        snapshot=monitor.snapshot,
        history=monitor.history,
        tick=lambda: None,
        commands=commands.surface(),
        replay=None,
        recording=None,
    )
    page = await open_page(user, sources)
    retry = next(iter(user.find(kind=ui.button, marker="retry").elements))
    # A live session offers no retry, and the control is not even built for
    # a replay.
    assert not retry.enabled
    user.find(kind=ui.button, marker="retry").click()
    assert commands.retries == 0
    commands.phase = Phase.REFUSED
    page.refresh()
    await user.should_see("commands: refused")
    assert retry.enabled
    user.find(kind=ui.button, marker="retry").click()
    assert commands.retries == 1


async def test_the_orientation_panel_renders_and_the_scene_follows_the_maths(
    user: User,
) -> None:
    monitor = fed(ROTATION_LINES)
    sources = Sources(
        label="replay session.txt",
        status=lambda: Status(State.ENDED),
        snapshot=monitor.snapshot,
        history=monitor.history,
        tick=lambda: None,
        commands=None,
        replay=FakeReplay().surface(),
        recording=None,
    )
    page = await open_page(user, sources)
    await user.should_see("Board orientation")
    await user.should_see("Euler angles")
    await user.should_see(reference_text(0x05))
    # The scene object carries the rotation matrix of the known sample,
    # written out here: the quaternion (0.24121, -0.03625, 0.03137,
    # -0.96930), normalised, turned into the matrix rotate_R takes.
    turn = (
        (-0.8810100708959423, 0.4653247280883376, 0.08540581016208505),
        (-0.4698732739562027, -0.8816700269872375, -0.04332516513533198),
        (0.055139472264491715, -0.07829981444319806, 0.9954038264224998),
    )
    assert [list(row) for row in turn] == page.board.R
    await user.should_see("yaw -151.93°, pitch -3.16°, roll -4.50°")
    # The device axes are drawn and labelled as part of the board group.
    labels = [
        obj.args[0] for obj in page.board.children if isinstance(obj.args[0], str)
    ]
    assert labels == ["X", "Y", "Z"]


async def test_switching_the_rotation_source_turns_the_board(user: User) -> None:
    monitor = fed(ROTATION_LINES)
    commands = FakeCommands()
    sources = Sources(
        label="board EXAMPLE-SERIAL",
        status=lambda: Status(State.CONNECTED),
        snapshot=monitor.snapshot,
        history=monitor.history,
        tick=lambda: None,
        commands=commands.surface(),
        replay=None,
        recording=None,
    )
    page = await open_page(user, sources)
    page.rotation_choice.value = 0x08
    page.refresh()
    await user.should_see(reference_text(0x08))
    # The written-out rotation of the known game sample, (0.99884,
    # -0.03918, -0.02765, 0.00110): a small turn about X and Y.
    turned = (
        (0.9984685076636275, -3.079454966731449e-05, -0.05532303548852197),
        (0.004364179898429098, 0.9969273803552756, 0.07820966840347618),
        (0.05515064041135604, -0.07833133057508512, 0.9954007281052963),
    )
    assert [list(row) for row in turned] == page.board.R
    # Choosing a source asks the board for nothing.
    assert commands.submitted == []
    assert set(rotation_options()) == {0x05, 0x08, 0x2A}


async def test_a_source_without_samples_names_the_sensor_to_enable(user: User) -> None:
    monitor = fed(ROTATION_LINES[:2])
    sources = Sources(
        label="replay session.txt",
        status=lambda: Status(State.ENDED),
        snapshot=monitor.snapshot,
        history=monitor.history,
        tick=lambda: None,
        commands=None,
        replay=FakeReplay().surface(),
        recording=None,
    )
    page = await open_page(user, sources)
    page.rotation_choice.value = 0x2A
    page.refresh()
    await user.should_see(missing_rotation_text(0x2A))
    await user.should_see("no samples of Gyro-integrated rotation vector (0x2A) yet")


async def test_the_motion_plots_follow_the_fed_samples(user: User) -> None:
    monitor = fed(
        f"V,1,{index},{4_000_000 + index * 250_000},2,0.10000,-0.20000,9.80000"
        for index in range(4)
    )
    sources = Sources(
        label="replay session.txt",
        status=lambda: Status(State.ENDED),
        snapshot=monitor.snapshot,
        history=monitor.history,
        tick=lambda: None,
        commands=None,
        replay=FakeReplay().surface(),
        recording=None,
    )
    clock = FakeClock()
    page = await open_page(user, sources, clock)
    chart = page.motion[0x01]
    series = cast("list[dict[str, object]]", chart.options["series"])
    assert [entry["name"] for entry in series] == ["x", "y", "z"]
    for axis, value in enumerate((0.1, -0.2, 9.8)):
        data = cast("list[list[float]]", series[axis]["data"])
        assert data == [
            [pytest.approx(4.0 + index * 0.25), pytest.approx(value)]
            for index in range(4)
        ]
    # A refresh without a new sample leaves the chart alone.
    chart.options["series"][0]["name"] = "sentinel"
    page.refresh()
    assert chart.options["series"][0]["name"] == "sentinel"
    # A new sample redraws it.
    monitor.on_line("V,1,4,5000000,2,0.10000,-0.20000,9.80000", 0.0)
    clock.now += panel.CHART_INTERVAL_SECONDS
    page.refresh()
    assert chart.options["series"][0]["name"] == "x"
    data = cast("list[list[float]]", chart.options["series"][0]["data"])
    assert data[-1] == [pytest.approx(5.0), pytest.approx(0.1)]


async def test_a_plot_without_samples_says_so_with_its_sensor(user: User) -> None:
    monitor = fed(
        (
            "Q,5,0,1000000,3,0.24121,-0.03625,0.03137,-0.96930,3.14160",
            "V,1,0,1000000,2,0.10000,-0.20000,9.80000",
        ),
    )
    sources = Sources(
        label="replay session.txt",
        status=lambda: Status(State.ENDED),
        snapshot=monitor.snapshot,
        history=monitor.history,
        tick=lambda: None,
        commands=None,
        replay=FakeReplay().surface(),
        recording=None,
    )
    page = await open_page(user, sources)
    await user.should_see(motion_window_text(0x01))
    assert page.motion_notes[0x01].text == motion_window_text(0x01)
    await user.should_see(no_motion_text(0x02))
    await user.should_see(no_motion_text(0x03))


async def test_nothing_on_the_new_panels_sends_a_command(user: User) -> None:
    monitor = fed(ROTATION_LINES)
    commands = FakeCommands()
    sources = Sources(
        label="board EXAMPLE-SERIAL",
        status=lambda: Status(State.CONNECTED),
        snapshot=monitor.snapshot,
        history=monitor.history,
        tick=lambda: None,
        commands=commands.surface(),
        replay=None,
        recording=None,
    )
    page = await open_page(user, sources)
    for sensor in (0x05, 0x08, 0x2A):
        page.rotation_choice.value = sensor
        page.refresh()
    assert commands.submitted == []


async def test_a_legacy_e_line_does_not_stop_the_page_refresh(user: User) -> None:
    """An E line in a plotted sensor's history leaves the page alive.

    The plots draw only payload lines (LP-6), so the legacy lines are
    skipped and every section `refresh` updates after them — the
    calibration, the response, the recording, the replay and the status
    line — still moves.
    """
    monitor = fed(
        (
            "# run 3000",
            "V,1,0,1000000,2,0.10000,-0.20000,9.80000",
            "E,1,1,1002000,3",
            "E,2,0,1003000,0",
            "E,3,0,1004000,0",
            "# cal status=0 accel=1 gyro=1 mag=1 planar=0 table=0",
            "# dcd status=0",
        ),
    )
    recording = FakeRecording()
    sources = Sources(
        label="replay session.txt",
        status=lambda: Status(State.ENDED),
        snapshot=monitor.snapshot,
        history=monitor.history,
        tick=lambda: None,
        commands=None,
        replay=FakeReplay().surface(),
        recording=recording.surface(),
    )
    page = await open_page(user, sources)
    chart = page.motion[0x01]
    data = cast("list[list[float]]", chart.options["series"][0]["data"])
    assert data == [[pytest.approx(1.0), pytest.approx(0.1)]]
    await user.should_see(no_motion_text(0x02))
    await user.should_see(no_motion_text(0x03))
    await user.should_see("status 0 (0 is success); accelerometer on")
    await user.should_see("last command response: # dcd status=0")
    await user.should_see("replay at 50%")
    await user.should_see("not recording")
    await user.should_see(summary(monitor.snapshot()))
    recording.start()
    page.refresh()
    await user.should_see("recording to session-20261125-160000.txt")


async def test_a_rotation_source_whose_latest_line_is_legacy_reads_no_orientation(
    user: User,
) -> None:
    """The latest sample can be an E line with no quaternion (LP-4).

    The readouts say the sample carries no orientation rather than
    raising, and the board keeps the turn of the payload line before it.
    """
    monitor = fed(
        ("Q,5,0,1000000,3,0.24121,-0.03625,0.03137,-0.96930,3.14160",),
    )
    sources = Sources(
        label="replay session.txt",
        status=lambda: Status(State.ENDED),
        snapshot=monitor.snapshot,
        history=monitor.history,
        tick=lambda: None,
        commands=None,
        replay=FakeReplay().surface(),
        recording=None,
    )
    page = await open_page(user, sources)
    turn = (
        (-0.8810100708959423, 0.4653247280883376, 0.08540581016208505),
        (-0.4698732739562027, -0.8816700269872375, -0.04332516513533198),
        (0.055139472264491715, -0.07829981444319806, 0.9954038264224998),
    )
    assert [list(row) for row in turn] == page.board.R
    monitor.on_line("E,5,1,1002000,0", 0.0)
    page.refresh()
    await user.should_see("the latest sample carries no orientation")
    assert [list(row) for row in turn] == page.board.R


async def test_the_page_timer_runs_at_the_refresh_interval(user: User) -> None:
    sources = Sources(
        label="replay session.txt",
        status=lambda: Status(State.ENDED),
        snapshot=Monitor().snapshot,
        history=Monitor().history,
        tick=lambda: None,
        commands=None,
        replay=None,
        recording=None,
    )
    await open_page(user, sources)
    assert user.client is not None
    timers = [
        element
        for element in user.client.elements.values()
        if isinstance(element, ui.timer)
    ]
    assert [timer.interval for timer in timers] == [REFRESH_INTERVAL_SECONDS]
    assert REFRESH_INTERVAL_SECONDS >= 1 / 30


async def test_a_refresh_lets_the_sources_check_their_timeouts(user: User) -> None:
    ticks: list[None] = []
    sources = Sources(
        label="replay session.txt",
        status=lambda: Status(State.ENDED),
        snapshot=Monitor().snapshot,
        history=Monitor().history,
        tick=lambda: ticks.append(None),
        commands=None,
        replay=None,
        recording=None,
    )
    page = await open_page(user, sources)
    before = len(ticks)
    page.refresh()
    assert len(ticks) == before + 1


async def test_a_chart_is_redrawn_at_most_ten_times_a_second(user: User) -> None:
    monitor = fed(("V,1,0,1000000,2,0.10000,-0.20000,9.80000",))
    sources = Sources(
        label="replay session.txt",
        status=lambda: Status(State.ENDED),
        snapshot=monitor.snapshot,
        history=monitor.history,
        tick=lambda: None,
        commands=None,
        replay=None,
        recording=None,
    )
    clock = FakeClock()
    page = await open_page(user, sources, clock)
    chart = page.motion[0x01]
    drawn = len(chart.options["series"][0]["data"])
    assert drawn == 1
    monitor.on_line("V,1,1,1002000,2,0.10000,-0.20000,9.80000", 0.0)
    clock.now += panel.CHART_INTERVAL_SECONDS / 2
    page.refresh()
    # The sample is new but the chart was drawn a moment ago.
    assert len(chart.options["series"][0]["data"]) == 1
    clock.now += panel.CHART_INTERVAL_SECONDS
    page.refresh()
    assert len(chart.options["series"][0]["data"]) == 2


async def test_a_table_is_sent_only_when_its_rows_changed(
    user: User,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monitor = fed(("V,1,0,1000000,2,0.10000,-0.20000,9.80000",))
    sources = Sources(
        label="replay session.txt",
        status=lambda: Status(State.ENDED),
        snapshot=monitor.snapshot,
        history=monitor.history,
        tick=lambda: None,
        commands=None,
        replay=None,
        recording=None,
    )
    page = await open_page(user, sources)
    sent: list[str] = []
    monkeypatch.setattr(page.sensors, "update", lambda: sent.append("sensors"))
    monkeypatch.setattr(page.health, "update", lambda: sent.append("health"))
    page.refresh()
    page.refresh()
    assert sent == []
    monitor.on_line("V,1,1,1002000,2,0.10000,-0.20000,9.80000", 0.0)
    page.refresh()
    assert sent == ["sensors", "health"]


def inside(element: ui.element, container: ui.element) -> bool:
    """Return whether `element` is `container` or one of its descendants."""
    current: ui.element | None = element
    while current is not None:
        if current is container:
            return True
        current = current.parent_slot.parent if current.parent_slot else None
    return False


def parent_row(element: ui.element) -> ui.element:
    """Return the row that holds the card around `element`."""
    current = element
    while not isinstance(current, ui.card):
        assert current.parent_slot is not None
        current = current.parent_slot.parent
    assert current.parent_slot is not None
    return current.parent_slot.parent


def tab_panel_of(user: User, element: ui.element) -> str | None:
    """Return the name of the tab panel that holds `element`, if any."""
    for panel_element in user.find(kind=ui.tab_panel).elements:
        if inside(element, panel_element):
            return str(panel_element.props["name"])
    return None


def tab_names(user: User) -> list[str]:
    """Return the names of the page's tabs, in order."""
    tabs = sorted(user.find(kind=ui.tab).elements, key=lambda tab: tab.id)
    return [str(tab.props["name"]) for tab in tabs]


def board_sources(replay: bool, recording: bool) -> Sources:
    """Return sources with a board, and a replay and a recording if asked."""
    monitor = replayed()
    return Sources(
        label="board EXAMPLE-SERIAL",
        status=lambda: Status(State.CONNECTED),
        snapshot=monitor.snapshot,
        history=monitor.history,
        tick=lambda: None,
        commands=FakeCommands().surface(),
        replay=FakeReplay().surface() if replay else None,
        recording=FakeRecording().surface() if recording else None,
    )


async def test_the_live_views_stay_outside_the_tabs(user: User) -> None:
    page = await open_page(user, board_sources(replay=True, recording=True))
    (panels,) = user.find(kind=ui.tab_panels).elements
    live: list[ui.element] = [
        page.controller,
        page.retry,
        page.rotation_choice,
        page.euler,
        *page.motion.values(),
    ]
    for element in live:
        assert not inside(element, panels)
    # The board view and the Euler readouts share a row; the plots share
    # another, one card each.
    assert parent_row(page.rotation_choice) is parent_row(page.euler)
    plots = [parent_row(chart) for chart in page.motion.values()]
    assert plots[0] is plots[1] is plots[2]
    assert plots[0] is not parent_row(page.euler)


async def test_each_control_group_sits_in_its_own_tab(user: User) -> None:
    page = await open_page(user, board_sources(replay=True, recording=True))
    assert tab_names(user) == [
        "sensors",
        "health",
        "calibration",
        "recording",
        "replay",
    ]
    expected: dict[str, list[ui.element]] = {
        "sensors": [page.sensors, page.enable, page.disable],
        "health": [page.health],
        "calibration": [page.tare, page.calibrate, page.calget, page.calibration],
        "recording": [page.record_start, page.record_stop],
        "replay": [page.hold, page.restart, page.progress],
    }
    for name, elements in expected.items():
        for element in elements:
            assert tab_panel_of(user, element) == name


async def test_the_recording_and_replay_tabs_need_their_sources(user: User) -> None:
    await open_page(user, board_sources(replay=False, recording=False))
    assert tab_names(user) == ["sensors", "health", "calibration"]


async def test_only_the_recording_tab_needs_a_recording(user: User) -> None:
    await open_page(user, board_sources(replay=False, recording=True))
    assert tab_names(user) == ["sensors", "health", "calibration", "recording"]


async def test_only_the_replay_tab_needs_a_replay(user: User) -> None:
    await open_page(user, board_sources(replay=True, recording=False))
    assert tab_names(user) == ["sensors", "health", "calibration", "replay"]


async def test_the_sensors_tab_is_shown_first(user: User) -> None:
    await open_page(user, board_sources(replay=True, recording=True))
    (panels,) = user.find(kind=ui.tab_panels).elements
    assert panels.value == "sensors"


async def test_selecting_a_tab_reaches_its_controls(user: User) -> None:
    commands = FakeCommands()
    monitor = replayed()
    sources = Sources(
        label="board EXAMPLE-SERIAL",
        status=lambda: Status(State.CONNECTED),
        snapshot=monitor.snapshot,
        history=monitor.history,
        tick=lambda: None,
        commands=commands.surface(),
        replay=None,
        recording=None,
    )
    page = await open_page(user, sources)
    (panels,) = user.find(kind=ui.tab_panels).elements
    user.find(kind=ui.tab, marker="tab-calibration").click()
    assert panels.value == "calibration"
    user.find(kind=ui.button, marker="tare").click()
    assert commands.submitted == [tare(0x7, 0)]
    user.find(kind=ui.tab, marker="tab-sensors").click()
    assert panels.value == "sensors"
    page.sensor_choice.value = 0x05
    user.find(kind=ui.button, marker="enable").click()
    assert commands.submitted[-1] == cfg(0x05, 10_000)
