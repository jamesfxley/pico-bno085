# SPDX-License-Identifier: MIT
import math
from pathlib import Path

import pytest

from pico_gui_bno085.protocol import (
    LINE_LIMIT,
    VALUE_NAMES,
    CalibrationStatus,
    Kind,
    ProtocolError,
    RawResponse,
    Reply,
    SaveDcdStatus,
    Telemetry,
    parse_line,
)

DATA = Path(__file__).resolve().parent.parent / "data" / "session.txt"


# LP-4: E,<sensor>,<seq>,<event_us>,<accuracy>.
def test_lp4_an_e_line_has_the_four_common_fields() -> None:
    assert parse_line("E,5,47,3786925165,0") == Telemetry(
        Kind.E,
        5,
        47,
        3786925165,
        0,
        (),
    )


# LP-6: each payload tag, with the fields after the common four.
def test_lp6_q_is_a_quaternion_then_accuracy() -> None:
    line = parse_line("Q,5,0,3777014117,0,0.24121,-0.03625,0.03137,-0.96930,3.14160")
    assert line == Telemetry(
        Kind.Q,
        5,
        0,
        3777014117,
        0,
        (0.24121, -0.03625, 0.03137, -0.96930, 3.1416),
    )


def test_lp6_r_is_a_quaternion_without_accuracy() -> None:
    line = parse_line("R,8,1,3777024197,3,0.99884,-0.03918,-0.02765,0.00110")
    assert line == Telemetry(
        Kind.R,
        8,
        1,
        3777024197,
        3,
        (0.99884, -0.03918, -0.02765, 0.0011),
    )


def test_lp6_v_is_x_y_z() -> None:
    line = parse_line("V,6,3,3780290313,3,0.52344,-0.76563,9.71875")
    assert line == Telemetry(Kind.V, 6, 3, 3780290313, 3, (0.52344, -0.76563, 9.71875))


def test_lp6_u_is_value_then_bias() -> None:
    line = parse_line(
        "U,15,10,3783627296,0,-43.31250,-90.43750,-97.25000,-19.81250,-52.68750,-46.62500",
    )
    assert isinstance(line, Telemetry)
    assert line.kind is Kind.U
    assert line.sensor == 15
    assert line.values == (-43.3125, -90.4375, -97.25, -19.8125, -52.6875, -46.625)


def test_lp6_g_is_a_quaternion_then_angular_velocity() -> None:
    line = parse_line(
        "G,42,0,3777015141,0,0.99884,-0.03918,-0.02771,0.00110,0.00879,-0.00293,-0.00488",
    )
    assert isinstance(line, Telemetry)
    assert line.kind is Kind.G
    assert len(line.values) == len(VALUE_NAMES[Kind.G]) == 7


def test_lp6_the_tag_and_not_the_sensor_selects_the_fields() -> None:
    line = parse_line("V,5,0,1,0,1.00000,2.00000,3.00000")
    assert isinstance(line, Telemetry)
    assert line.kind is Kind.V
    assert line.sensor == 5


def test_lp6_nan_is_a_value() -> None:
    line = parse_line("V,1,0,1,0,nan,0.00000,0.00000")
    assert isinstance(line, Telemetry)
    assert math.isnan(line.values[0])


def test_lp6_a_negative_value_rounded_to_zero_prints_without_a_sign() -> None:
    line = parse_line("V,1,0,1,0,0.00000,0.00000,0.00000")
    assert isinstance(line, Telemetry)
    assert line.values == (0.0, 0.0, 0.0)


@pytest.mark.parametrize(
    "value",
    [
        "1.0",
        "1.000000",
        "1",
        ".00001",
        "+1.00000",
        "1e3",
        "1.0000e0",
        "-nan",
        "NaN",
        "1234567890.00000",
        "--1.00000",
    ],
)
def test_lp6_a_number_not_in_the_format_is_refused(value: str) -> None:
    with pytest.raises(ProtocolError, match="five decimals"):
        parse_line(f"V,1,0,1,0,{value},0.00000,0.00000")


def test_lp6_a_magnitude_below_one_billion_is_accepted() -> None:
    line = parse_line("V,1,0,1,0,999999999.99999,0.00000,0.00000")
    assert isinstance(line, Telemetry)
    assert line.values[0] == 999999999.99999


@pytest.mark.parametrize(
    "text",
    [
        "Q,5,0,1,0,0.00000,0.00000,0.00000,1.00000",  # one field short
        "Q,5,0,1,0,0.00000,0.00000,0.00000,1.00000,0.50000,0.00000",  # one over
        "R,8,0,1,0,0.00000,0.00000,0.00000,1.00000,0.50000",  # R has no accuracy
        "E,5,0,1",
        "E,5,0,1,0,0.00000",
        "V,1,0,1,0,1.00000,2.00000",
        "U,7,0,1,0,1.00000,2.00000,3.00000",
        "G,42,0,1,0,1.00000,2.00000,3.00000,4.00000,5.00000,6.00000",
    ],
)
def test_lp6_a_wrong_field_count_is_refused(text: str) -> None:
    with pytest.raises(ProtocolError, match="fields"):
        parse_line(text)


@pytest.mark.parametrize(
    ("text", "match"),
    [
        ("E,x,0,1,0", "sensor"),
        ("E,256,0,1,0", "sensor 256 is above 255"),
        ("E,5,256,1,0", "sequence 256 is above 255"),
        ("E,5,-1,1,0", "sequence"),
        ("E,5,0,1.5,0", "time"),
        ("E,5,0,,0", "time"),
        ("E,5,0,1,4", "accuracy 4 is above 3"),
        ("E,5,0,1,", "accuracy"),
        ("E,\u0665,0,1,0", "sensor"),
        ("E,5,0,1,0 ", "accuracy"),
    ],
)
def test_lp4_a_malformed_common_field_is_refused(text: str, match: str) -> None:
    with pytest.raises(ProtocolError, match=match):
        parse_line(text)


def e_line_of_length(length: int) -> str:
    prefix, suffix = "E,5,0,", ",0"
    return prefix + "1".rjust(length - len(prefix) - len(suffix), "0") + suffix


def test_lp1_a_telemetry_line_of_127_characters_is_accepted() -> None:
    text = e_line_of_length(127)
    assert len(text) == 127
    assert isinstance(parse_line(text), Telemetry)


def test_lp1_a_telemetry_line_over_127_characters_is_refused() -> None:
    text = e_line_of_length(128)
    assert len(text) == 128
    with pytest.raises(ProtocolError, match="127"):
        parse_line(text)


# LP-3 and LP-1: replies begin "# ", with words and key=value pairs.
def test_lp3_a_bare_reply_is_a_word() -> None:
    assert parse_line("# ready") == Reply("ready", (), {})


def test_lp3_a_reply_keeps_positional_words() -> None:
    assert parse_line("# bus spi") == Reply("bus", ("spi",), {})
    assert parse_line("# err bus must be i2c or spi") == Reply(
        "err",
        ("bus", "must", "be", "i2c", "or", "spi"),
        {},
    )


def test_lp3_info_carries_key_value_pairs() -> None:
    line = parse_line("# info tick_us=1 max_enabled=13 bus=none")
    assert line == Reply(
        "info",
        (),
        {"tick_us": "1", "max_enabled": "13", "bus": "none"},
    )


def test_lp3_a_stats_line_longer_than_a_telemetry_line_is_a_reply() -> None:
    text = "# stats bus=spi " + " ".join(f"k{n}={n}" for n in range(120))
    assert len(text) > 127
    line = parse_line(text)
    assert isinstance(line, Reply)
    assert len(line.fields) == 121


def test_lp3_a_reply_with_an_empty_word_is_refused() -> None:
    for text in ("# ", "#  ready", "# ready ", "# run  3000"):
        with pytest.raises(ProtocolError, match="empty"):
            parse_line(text)


def test_lp1_a_field_without_a_name_or_a_repeated_field_is_refused() -> None:
    with pytest.raises(ProtocolError, match="no name"):
        parse_line("# info =1")
    with pytest.raises(ProtocolError, match="twice"):
        parse_line("# info a=1 a=2")


def test_lp1_a_line_over_the_limit_is_refused() -> None:
    with pytest.raises(ProtocolError, match=str(LINE_LIMIT)):
        parse_line("# stats " + "x" * LINE_LIMIT)


# LP-7: command responses.
def test_lp7_save_dcd_reports_its_status() -> None:
    assert parse_line("# dcd status=0") == SaveDcdStatus(0)


def test_lp7_me_calibration_reports_each_routine() -> None:
    line = parse_line("# cal status=0 accel=1 gyro=0 mag=1 planar=0 table=0")
    assert line == CalibrationStatus(0, 1, 0, 1, 0, 0)


def test_lp7_any_other_response_prints_its_raw_result() -> None:
    line = parse_line("# resp cmd=0c seq=4 rseq=1 r=0001020304050607080a0b")
    assert line == RawResponse(
        0x0C,
        4,
        1,
        bytes([0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11]),
    )


@pytest.mark.parametrize(
    "text",
    [
        "# dcd",
        "# dcd status=x",
        "# dcd status=256",
        "# dcd status=0 extra=1",
        "# dcd status=0 extra",
        "# cal status=0 accel=1 gyro=0 mag=1 planar=0",
        "# cal status=0 accel=1 mag=1 gyro=0 planar=0 table=0",
        "# cal status=0 accel=1 gyro=0 mag=1 planar=0 table=300",
        "# resp cmd=0c seq=4 rseq=1 r=0001020304050607080a",
        "# resp cmd=0c seq=4 rseq=1 r=0001020304050607080a0B",
        "# resp cmd=c seq=4 rseq=1 r=0001020304050607080a0b",
        "# resp cmd=0c seq=4 rseq=1",
        "# resp cmd=0c seq=x rseq=1 r=0001020304050607080a0b",
    ],
)
def test_lp7_a_malformed_response_is_refused(text: str) -> None:
    with pytest.raises(ProtocolError, match=r"LP-7|decimal|above"):
        parse_line(text)


# Lines the protocol does not define.
@pytest.mark.parametrize(
    "text",
    [
        "",
        "ready",
        "#ready",
        "X,1,2,3,4",
        "q,5,0,1,0,0.00000,0.00000,0.00000,1.00000,0.50000",
        "Q5,0,1,0",
        "E",
        "1970-01-01 01:02:56.733 [\x1b[32mINFO\x1b[0m] bno085.cpp:40: bno085: begin",
    ],
)
def test_a_line_that_is_not_a_protocol_line_is_refused(text: str) -> None:
    with pytest.raises(ProtocolError):
        parse_line(text)


def test_every_line_of_a_recorded_session_parses() -> None:
    lines = DATA.read_text(encoding="ascii").splitlines()
    assert len(lines) > 100
    parsed = [parse_line(line) for line in lines]
    assert {type(line) for line in parsed} == {
        Telemetry,
        Reply,
        CalibrationStatus,
        SaveDcdStatus,
    }
    kinds = {line.kind for line in parsed if isinstance(line, Telemetry)}
    assert kinds == set(Kind)
