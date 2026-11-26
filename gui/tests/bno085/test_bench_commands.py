# SPDX-License-Identifier: MIT
import pytest

from pico_gui.commands import CommandError
from pico_gui_bno085.commands import (
    MAX_RATE_HZ,
    UNSIGNED_MAX,
    cal,
    calget,
    cfg,
    connect_bus,
    connect_payload,
    off,
    run,
    tare,
)


def test_the_connect_commands_name_their_replies() -> None:
    assert connect_bus("spi") == connect_bus("spi")
    assert connect_bus("spi").line == "bus spi"
    assert connect_bus("spi").success == "# bus spi"
    assert connect_payload().line == "payload 1"
    assert connect_payload().success == "# payload 1"


def test_a_bus_that_is_neither_choice_is_refused() -> None:
    with pytest.raises(CommandError, match="neither i2c nor spi"):
        connect_bus("can")


def test_a_cfg_line_carries_its_sensor_and_interval() -> None:
    command = cfg(0x0F, 10_000)
    assert command.line == "cfg 0f 10000"
    assert command.success == "# ok"
    assert (command.sensor, command.interval_us, command.enables) == (
        0x0F,
        10_000,
        True,
    )


def test_a_sensor_above_a_byte_is_refused() -> None:
    with pytest.raises(CommandError, match="not a byte"):
        cfg(0x100, 10_000)
    with pytest.raises(CommandError, match="not a byte"):
        off(-1)


def test_an_interval_the_bench_parser_refuses_is_refused() -> None:
    with pytest.raises(CommandError, match="unsigned 32-bit"):
        cfg(0x01, UNSIGNED_MAX + 1)
    with pytest.raises(CommandError, match="unsigned 32-bit"):
        run(UNSIGNED_MAX + 1)


def test_an_interval_of_zero_is_refused() -> None:
    """The firmware parser accepts zero, but 1000-3625 §5.4.1 makes it off.

    A `cfg` of zero would turn the sensor off while the bench keeps its
    desired entry, so the GUI refuses it and disables with `off`.
    """
    with pytest.raises(CommandError, match="above zero"):
        cfg(0x01, 0)


def test_the_interval_bounds_run_from_one_to_the_unsigned_maximum() -> None:
    assert cfg(0x01, 1).line == "cfg 01 1"
    assert cfg(0x01, UNSIGNED_MAX).line == f"cfg 01 {UNSIGNED_MAX}"


def test_an_off_line_names_its_sensor() -> None:
    command = off(0x2A)
    assert command.line == "off 2a"
    assert (command.sensor, command.enables) == (0x2A, False)


def test_a_run_line_is_marked_as_the_command_that_streams() -> None:
    command = run(1000)
    assert command.line == "run 1000"
    assert command.success == "# run"
    assert command.run


def test_tare_takes_the_axes_and_basis_the_bench_accepts() -> None:
    command = tare(0x7, 0)
    assert command.line == "tare 7 0"
    assert command.success == "# ok"
    assert command.needs_ready


def test_tare_refuses_axes_and_bases_it_would_refuse() -> None:
    with pytest.raises(CommandError, match="neither 7"):
        tare(0x5, 0)
    with pytest.raises(CommandError, match="between 0 and 5"):
        tare(0x4, 6)


def test_cal_builds_the_three_routine_switches() -> None:
    assert cal(True, False, True).line == "cal 1 0 1"
    assert cal(False, False, False).line == "cal 0 0 0"
    assert cal(True, True, True).needs_ready


def test_calget_needs_a_ready_session() -> None:
    assert calget().line == "calget"
    assert calget().needs_ready


def test_every_sensor_of_the_lp8_table_has_a_maximum_rate() -> None:
    lp8: dict[int, int] = {
        0x2A: 1000,
        0x05: 400,
        0x28: 400,
        0x08: 400,
        0x29: 400,
        0x06: 400,
        0x04: 400,
        0x01: 500,
        0x02: 400,
        0x07: 400,
        0x03: 100,
        0x0F: 100,
        0x09: 90,
    }
    assert lp8 == MAX_RATE_HZ
