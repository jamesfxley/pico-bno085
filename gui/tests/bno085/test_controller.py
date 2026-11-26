# SPDX-License-Identifier: MIT
import threading

import pytest

from pico_gui.commands import CommandError
from pico_gui.transport import TransportError
from pico_gui_bno085.commands import DeviceCommand, cal, calget, cfg, off, tare
from pico_gui_bno085.controller import Controller, Phase

WAIT = 5.0


class ScriptedSend:
    """A send callable that records the bytes and can fail on demand."""

    def __init__(self, *, fail: bool = False) -> None:
        self.lines: list[bytes] = []
        self.fail = fail

    def __call__(self, data: bytes) -> None:
        if self.fail:
            raise TransportError("the stream is gone")
        self.lines.append(data)


def connected(
    send: ScriptedSend,
    *,
    bus: str = "spi",
    run_ms: int = 1000,
    reply_timeout: float = 2.0,
    run_grace: float = 2.0,
) -> Controller:
    controller = Controller(
        send,
        bus,
        run_ms=run_ms,
        reply_timeout=reply_timeout,
        run_grace=run_grace,
    )
    controller.start(0.0)
    controller.on_line(f"# bus {bus}", 0.1)
    controller.on_line("# payload 1", 0.2)
    return controller


def lines(send: ScriptedSend) -> list[str]:
    return [line.decode("ascii").rstrip("\n") for line in send.lines]


def test_the_connect_sequence_waits_for_each_reply() -> None:
    send = ScriptedSend()
    controller = Controller(send, "i2c")
    controller.start(0.0)
    assert lines(send) == ["bus i2c"]
    controller.on_line("# ready", 0.05)
    assert lines(send) == ["bus i2c"]
    controller.on_line("# bus i2c", 0.1)
    assert lines(send) == ["bus i2c", "payload 1"]
    controller.on_line("# payload 1", 0.2)
    assert controller.state().phase is Phase.LIVE


def test_a_refused_bus_refuses_the_session_and_every_command() -> None:
    send = ScriptedSend()
    controller = Controller(send, "spi")
    controller.start(0.0)
    controller.on_line("# err transport init", 0.1)
    state = controller.state()
    assert state.phase is Phase.REFUSED
    assert "was refused" in state.detail
    assert lines(send) == ["bus spi"]
    with pytest.raises(CommandError, match="bus never came up"):
        controller.submit(cfg(0x05, 10_000), 0.2)


def test_a_refused_payload_still_leaves_the_session_live() -> None:
    send = ScriptedSend()
    controller = Controller(send, "spi")
    controller.start(0.0)
    controller.on_line("# bus spi", 0.1)
    controller.on_line("# err", 0.2)
    state = controller.state()
    assert state.phase is Phase.LIVE
    assert "was refused" in state.detail


def test_an_enabled_sensor_starts_a_run_that_keeps_restarting() -> None:
    send = ScriptedSend()
    controller = connected(send)
    controller.submit(cfg(0x05, 10_000), 0.3)
    assert lines(send)[-1] == "cfg 05 10000"
    controller.on_line("# ok", 0.4)
    state = controller.state()
    assert state.enabled == {0x05}
    assert state.commanded == {0x05: 10_000}
    assert lines(send)[-1] == "run 1000"
    controller.on_line("# run 1000", 0.5)
    assert controller.state().streaming
    controller.on_line("Q,5,0,100,3,0.0,0.0,0.0,1.0,0.0", 0.6)
    controller.on_line("# done events=1", 1.5)
    assert not controller.state().streaming
    assert lines(send)[-1] == "run 1000"


def test_the_run_restarts_after_each_done_only_while_a_sensor_is_enabled() -> None:
    send = ScriptedSend()
    controller = connected(send)
    controller.submit(cfg(0x01, 5_000), 0.3)
    controller.on_line("# ok", 0.4)
    controller.on_line("# run 1000", 0.5)
    controller.submit(off(0x01), 0.6)
    controller.on_line("# ok", 0.7)
    assert controller.state().enabled == set()
    controller.on_line("# done events=1", 1.5)
    assert lines(send)[-1] == "off 01"


def test_a_mid_run_command_is_sent_at_once() -> None:
    send = ScriptedSend()
    controller = connected(send)
    controller.submit(cfg(0x05, 10_000), 0.3)
    controller.on_line("# ok", 0.4)
    controller.on_line("# run 1000", 0.5)
    controller.submit(off(0x05), 0.6)
    assert lines(send)[-1] == "off 05"
    controller.on_line("# ok", 0.7)
    assert controller.state().enabled == set()


def test_commands_one_at_a_time_each_matched_to_its_reply() -> None:
    send = ScriptedSend()
    controller = connected(send)
    controller.submit(cfg(0x05, 10_000), 0.3)
    controller.submit(off(0x05), 0.3)
    assert lines(send)[-1] == "cfg 05 10000"
    controller.on_line("# ok", 0.4)
    assert lines(send)[-1] == "off 05"
    controller.on_line("# ok", 0.5)
    assert lines(send)[-1] == "run 1000"


def test_a_refused_command_is_the_detail_and_the_next_one_goes_out() -> None:
    send = ScriptedSend()
    controller = connected(send)
    controller.submit(cfg(0x05, 10_000), 0.3)
    controller.on_line("# err", 0.4)
    state = controller.state()
    assert "was refused" in state.detail
    assert state.enabled == set()
    assert lines(send)[-1] == "cfg 05 10000"


def test_calibration_commands_wait_until_the_hub_is_ready() -> None:
    send = ScriptedSend()
    controller = connected(send)
    controller.submit(tare(0x7, 0x2), 0.3)
    assert lines(send) == ["bus spi", "payload 1"]
    assert "waiting for the hub" in controller.state().detail
    controller.on_line("# ready", 0.4)
    assert lines(send)[-1] == "tare 7 2"
    controller.on_line("# ok", 0.5)
    controller.submit(calget(), 0.6)
    assert lines(send)[-1] == "calget"


def test_a_mid_session_ready_line_keeps_the_stream_running() -> None:
    send = ScriptedSend()
    controller = connected(send)
    controller.submit(cfg(0x05, 10_000), 0.3)
    controller.on_line("# ok", 0.4)
    controller.on_line("# run 1000", 0.5)
    controller.on_line("# ready", 0.6)
    state = controller.state()
    assert state.streaming
    assert state.ready


def test_a_missing_reply_is_the_detail_after_the_timeout() -> None:
    send = ScriptedSend()
    controller = connected(send, reply_timeout=2.0)
    controller.submit(off(0x05), 10.0)
    assert lines(send)[-1] == "off 05"
    controller.tick(11.9)
    assert "no reply" not in controller.state().detail
    controller.tick(12.1)
    state = controller.state()
    assert "no reply to 'off 05'" in state.detail


def test_a_lost_done_line_restarts_the_run() -> None:
    send = ScriptedSend()
    controller = connected(send, run_ms=1000, run_grace=2.0)
    controller.submit(cfg(0x05, 10_000), 0.3)
    controller.on_line("# ok", 0.4)
    controller.on_line("# run 1000", 0.5)
    count = len(send.lines)
    controller.tick(3.4)
    assert len(send.lines) == count
    controller.tick(3.6)
    state = controller.state()
    assert "without its done line" in state.detail
    assert len(send.lines) == count + 1
    assert lines(send)[-1] == "run 1000"


def test_a_run_whose_reply_never_comes_is_asked_again() -> None:
    send = ScriptedSend()
    controller = connected(send, reply_timeout=2.0)
    controller.submit(cfg(0x05, 10_000), 0.3)
    controller.on_line("# ok", 0.4)
    assert lines(send)[-1] == "run 1000"
    controller.tick(2.5)
    assert "no reply to 'run 1000'" in controller.state().detail
    assert lines(send)[-1] == "run 1000"


def test_a_send_failure_is_the_detail_and_the_queue_is_given_up() -> None:
    send = ScriptedSend(fail=True)
    controller = Controller(send, "spi")
    controller.start(0.0)
    state = controller.state()
    assert state.phase is Phase.REFUSED
    assert "cannot send" in state.detail


def test_a_command_whose_argument_the_bench_refuses_is_refused() -> None:
    send = ScriptedSend()
    controller = connected(send)
    controller.submit(cal(True, True, True), 0.3)
    with pytest.raises(CommandError, match="32-bit"):
        controller.submit(cfg(0x05, 10_000_000_000_000_000_000), 0.3)


def test_a_line_the_protocol_refuses_is_not_written() -> None:
    send = ScriptedSend()
    controller = connected(send)
    sent = list(send.lines)
    with pytest.raises(CommandError, match="empty"):
        controller.submit(DeviceCommand("", "# ok"), 0.3)
    assert send.lines == sent


def test_a_reply_after_a_timeout_is_treated_as_possibly_stale() -> None:
    send = ScriptedSend()
    controller = connected(send, reply_timeout=2.0)
    controller.submit(cfg(0x05, 10_000), 10.0)
    assert lines(send)[-1] == "cfg 05 10000"
    controller.tick(12.1)
    assert "no reply to 'cfg 05 10000'" in controller.state().detail
    controller.submit(cfg(0x01, 5_000), 13.0)
    assert lines(send)[-1] == "cfg 01 5000"
    controller.on_line("# ok", 13.1)
    state = controller.state()
    # The reply may answer the cfg that timed out, so no sensor is marked
    # enabled by it, and the cfg in flight keeps its turn.
    assert "may answer 'cfg 05 10000'" in state.detail
    assert state.enabled == set()
    assert state.in_flight == "cfg 01 5000"
    # The cfg's own reply, when it comes, is applied normally again.
    controller.on_line("# ok", 13.2)
    state = controller.state()
    assert state.enabled == {0x01}
    assert state.commanded == {0x01: 5_000}


def test_a_late_reply_with_nothing_in_flight_clears_the_suspicion() -> None:
    send = ScriptedSend()
    controller = connected(send, reply_timeout=2.0)
    controller.submit(cfg(0x05, 10_000), 10.0)
    controller.tick(12.1)
    # The late reply arrives before the next command is sent, so it is
    # dropped and the next command's reply is not suspect.
    controller.on_line("# ok", 12.2)
    state = controller.state()
    assert "answered 'cfg 05 10000'" in state.detail
    assert state.enabled == set()
    controller.submit(off(0x05), 13.0)
    controller.on_line("# ok", 13.1)
    assert controller.state().enabled == set()
    assert "may answer" not in controller.state().detail


def test_a_bus_timeout_leaves_a_retry_and_a_late_bus_answer_resumes() -> None:
    send = ScriptedSend()
    controller = Controller(send, "spi", reply_timeout=2.0)
    controller.start(0.0)
    assert lines(send) == ["bus spi"]
    controller.tick(2.1)
    state = controller.state()
    assert state.phase is Phase.REFUSED
    assert "no reply to 'bus spi'" in state.detail
    # The late answer brings the session up without a retry: the payload
    # follows at once.
    controller.on_line("# bus spi", 2.2)
    assert "arrived late" in controller.state().detail
    assert lines(send) == ["bus spi", "payload 1"]
    controller.on_line("# payload 1", 2.3)
    assert controller.state().phase is Phase.LIVE


def test_a_retry_after_a_send_failure_asks_for_the_bus_again() -> None:
    send = ScriptedSend()
    send.fail = True
    controller = Controller(send, "i2c")
    controller.start(0.0)
    assert controller.state().phase is Phase.REFUSED
    assert "cannot send" in controller.state().detail
    send.fail = False
    controller.retry_connect(1.0)
    # One command goes out at a time; the payload follows the bus reply.
    assert lines(send) == ["bus i2c"]
    controller.on_line("# bus i2c", 1.1)
    assert lines(send) == ["bus i2c", "payload 1"]
    controller.on_line("# payload 1", 1.2)
    assert controller.state().phase is Phase.LIVE


def test_a_retry_keeps_the_sensors_the_session_still_commands() -> None:
    send = ScriptedSend()
    controller = connected(send)
    controller.submit(cfg(0x05, 10_000), 0.3)
    controller.on_line("# ok", 0.4)
    controller.on_line("# run 1000", 0.5)
    assert controller.state().enabled == {0x05}
    # The stream stalls: the run's re-arm fails to send and the command
    # path is refused.
    send.fail = True
    controller.on_line("# done events=1", 1.5)
    assert controller.state().phase is Phase.REFUSED
    assert "cannot send 'run 1000'" in controller.state().detail
    send.fail = False
    controller.retry_connect(3.0)
    state = controller.state()
    # The board still holds the sensor as desired and re-enables it on a
    # bus retry, so the session's knowledge stays as it was.
    assert state.enabled == {0x05}
    assert state.commanded == {0x05: 10_000}
    assert lines(send)[-1] == "bus spi"


def test_a_retry_on_a_live_session_changes_nothing() -> None:
    send = ScriptedSend()
    controller = connected(send)
    controller.submit(cfg(0x05, 10_000), 0.3)
    controller.on_line("# ok", 0.4)
    count = len(send.lines)
    controller.retry_connect(0.5)
    assert len(send.lines) == count
    assert controller.state().phase is Phase.LIVE


def test_a_stalled_send_does_not_block_the_state() -> None:
    started = threading.Event()
    release = threading.Event()

    def send(_data: bytes) -> None:
        started.set()
        assert release.wait(WAIT)

    controller = Controller(send, "spi")
    stopper = threading.Thread(target=controller.start, args=(0.0,))
    stopper.start()
    assert started.wait(WAIT)
    # The write is in flight on the stopper's thread and holds no lock, so
    # the state still answers while the stream is stalled.
    state = controller.state()
    assert state.in_flight == "bus spi"
    release.set()
    stopper.join(WAIT)
    assert not stopper.is_alive()


def test_a_reply_the_timed_out_command_could_not_have_sent_is_applied() -> None:
    send = ScriptedSend()
    controller = Controller(send, "spi", reply_timeout=2.0)
    controller.start(0.0)
    controller.on_line("# bus spi", 0.1)
    controller.tick(2.2)
    # The payload's wait passed, so the session is live on its working bus;
    # a payload reply has the form "# payload 1", so a later "# ok" cannot
    # answer it and serves the cfg in flight.
    assert controller.state().phase is Phase.LIVE
    controller.submit(cfg(0x05, 10_000), 3.0)
    controller.on_line("# ok", 3.1)
    state = controller.state()
    assert state.enabled == {0x05}
    assert state.commanded == {0x05: 10_000}
    assert lines(send)[-1] == "run 1000"


def test_two_consecutive_timeouts_do_not_wedge_the_command_path() -> None:
    send = ScriptedSend()
    controller = connected(send, reply_timeout=2.0)
    controller.submit(cfg(0x05, 10_000), 10.0)
    controller.tick(12.1)
    controller.submit(cfg(0x01, 5_000), 13.0)
    controller.tick(15.1)
    assert "no reply to 'cfg 01 5000'" in controller.state().detail
    controller.submit(cfg(0x02, 5_000), 16.0)
    controller.on_line("# ok", 16.1)
    # Dropped: it may answer the cfg 01 whose wait passed. The cfg in flight
    # keeps its turn, and when its own wait passes its outcome is unknown:
    # its reply may be the one already dropped, so no new suspicion stands.
    assert controller.state().enabled == set()
    controller.tick(18.2)
    state = controller.state()
    assert "outcome of 'cfg 02 5000' is unknown" in state.detail
    # The cfg 01 still owes its late answer, so one further tick closes its
    # own window before the path serves the next command normally.
    controller.tick(19.2)
    controller.submit(cfg(0x03, 5_000), 20.0)
    controller.on_line("# ok", 20.1)
    assert controller.state().enabled == {0x03}


def test_a_lost_cfg_reply_then_prompt_cfgs_recover_the_path() -> None:
    send = ScriptedSend()
    controller = connected(send, reply_timeout=2.0)
    controller.submit(cfg(0x05, 10_000), 10.0)
    controller.tick(12.1)
    controller.submit(cfg(0x01, 5_000), 13.0)
    # Dropped as the late answer to the cfg whose wait passed.
    controller.on_line("# ok", 13.1)
    assert controller.state().enabled == set()
    controller.submit(cfg(0x02, 5_000), 14.0)
    controller.tick(15.2)
    # The cfg 01's reply was the dropped one, so its outcome is unknown and
    # the cfg 02's own reply is taken normally: the path has recovered.
    controller.on_line("# ok", 16.0)
    state = controller.state()
    assert state.enabled == {0x02}
    assert lines(send)[-1] == "run 1000"


def test_the_suspicion_expires_when_no_reply_arrives() -> None:
    send = ScriptedSend()
    controller = connected(send, reply_timeout=2.0)
    controller.submit(cfg(0x05, 10_000), 10.0)
    controller.tick(12.1)
    assert "no reply to 'cfg 05 10000'" in controller.state().detail
    # More than one further wait passes with no late reply, so the
    # suspicion ends and the next reply serves its own command.
    controller.tick(16.3)
    controller.submit(cfg(0x01, 5_000), 17.0)
    controller.on_line("# ok", 17.1)
    assert controller.state().enabled == {0x01}


def test_asynchronous_lines_leave_the_suspicion_standing() -> None:
    send = ScriptedSend()
    controller = connected(send, reply_timeout=2.0)
    controller.submit(cfg(0x05, 10_000), 10.0)
    controller.tick(12.1)
    controller.submit(cfg(0x01, 5_000), 13.0)
    # Telemetry and the done line are not replies: the suspicion still
    # stands, so the late ok of the cfg that timed out is dropped and the
    # cfg in flight awaits its own.
    controller.on_line("R,8,1,1000,3,0.00000,0.00000,0.00000,1.00000", 13.05)
    controller.on_line("# done events=1", 13.06)
    controller.on_line("# ok", 13.1)
    state = controller.state()
    assert "may answer 'cfg 05 10000'" in state.detail
    assert state.enabled == set()
    assert state.in_flight == "cfg 01 5000"
    controller.on_line("# ok", 13.2)
    assert controller.state().enabled == {0x01}


def test_two_timeouts_allow_two_dropped_replies() -> None:
    send = ScriptedSend()
    controller = connected(send, reply_timeout=2.0)
    controller.submit(cfg(0x05, 10_000), 10.0)
    controller.tick(12.1)
    controller.submit(cfg(0x01, 5_000), 13.0)
    controller.tick(15.1)
    controller.submit(cfg(0x02, 5_000), 16.0)
    # Both waits passed, so both late answers are owed and each is dropped
    # for its own entry, oldest first.
    controller.on_line("# ok", 16.1)
    assert controller.state().enabled == set()
    controller.on_line("# ok", 16.2)
    state = controller.state()
    assert state.enabled == set()
    assert state.in_flight == "cfg 02 5000"
    # The command in flight keeps its turn for its own reply.
    controller.on_line("# ok", 16.3)
    assert controller.state().enabled == {0x02}


def test_a_reply_of_another_form_judges_the_oldest_entry_lost() -> None:
    send = ScriptedSend()
    controller = Controller(send, "spi", reply_timeout=2.0)
    controller.start(0.0)
    controller.on_line("# bus spi", 0.1)
    controller.tick(2.2)
    controller.submit(cfg(0x05, 10_000), 3.0)
    controller.tick(5.1)
    controller.submit(cfg(0x01, 5_000), 6.0)
    # The payload's reply never came, so an "# ok" cannot answer it: that
    # entry is judged lost and the "# ok" is tested against the cfg, whose
    # reply form it has, and dropped for it.
    controller.on_line("# ok", 6.1)
    state = controller.state()
    assert "may answer 'cfg 05 10000'" in state.detail
    assert state.enabled == set()
    controller.on_line("# ok", 6.2)
    assert controller.state().enabled == {0x01}


def test_ordered_late_replies_of_two_forms_are_both_dropped() -> None:
    send = ScriptedSend()
    controller = Controller(send, "spi", reply_timeout=2.0)
    controller.start(0.0)
    controller.on_line("# bus spi", 0.1)
    controller.tick(2.2)
    controller.submit(cfg(0x05, 10_000), 3.0)
    controller.tick(5.1)
    controller.submit(cfg(0x01, 5_000), 6.0)
    # The payload's late answer and the cfg's late answer both arrive, in
    # the order the bench sends them, and each is dropped for its own
    # entry before the cfg in flight is answered.
    controller.on_line("# payload 1", 6.1)
    state = controller.state()
    assert state.in_flight == "cfg 01 5000"
    controller.on_line("# ok", 6.2)
    assert controller.state().enabled == set()
    assert controller.state().in_flight == "cfg 01 5000"
    controller.on_line("# ok", 6.3)
    assert controller.state().enabled == {0x01}


def test_entries_expire_one_by_one() -> None:
    send = ScriptedSend()
    controller = Controller(send, "spi", reply_timeout=2.0)
    controller.start(0.0)
    controller.on_line("# bus spi", 0.1)
    controller.tick(2.2)
    controller.submit(cfg(0x05, 10_000), 3.0)
    controller.tick(5.1)
    controller.submit(cfg(0x01, 5_000), 6.0)
    # The payload's wait passed more than two waits ago and the cfg's only
    # 1.4 s, so the tick removes the older entry alone: the cfg's late
    # answer is still dropped, and the payload's own late reply, arriving
    # after its entry left, serves nothing.
    controller.tick(6.5)
    controller.on_line("# ok", 6.6)
    state = controller.state()
    assert "may answer 'cfg 05 10000'" in state.detail
    assert state.enabled == set()
    controller.on_line("# ok", 6.7)
    assert controller.state().enabled == {0x01}
    controller.on_line("# payload 1", 6.8)
    state = controller.state()
    assert state.enabled == {0x01}


def test_a_retry_clears_the_suspects() -> None:
    send = ScriptedSend()
    controller = Controller(send, "spi", reply_timeout=2.0)
    controller.start(0.0)
    controller.tick(2.1)
    # The bus's wait passed, so the bus is the suspect and the session is
    # refused.
    assert controller.state().phase is Phase.REFUSED
    controller.retry_connect(3.0)
    # The retry cleared the suspects, so the bus answer serves the retried
    # bus command instead of being dropped as the timed-out one's late
    # answer.
    controller.on_line("# bus spi", 3.1)
    assert lines(send) == ["bus spi", "bus spi", "payload 1"]
    controller.on_line("# payload 1", 3.2)
    assert controller.state().phase is Phase.LIVE
    controller.submit(cfg(0x01, 5_000), 4.0)
    controller.on_line("# ok", 4.1)
    assert controller.state().enabled == {0x01}
