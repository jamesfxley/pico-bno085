# SPDX-License-Identifier: MIT
"""Parse bench protocol lines into typed records (LP-1 to LP-7).

The parser is pure. It takes one line of text and returns a record or raises
`ProtocolError`. It does not read a stream.
"""

import re
from collections.abc import Mapping
from dataclasses import dataclass
from enum import Enum

# LP-1: a telemetry line or command response is at most 127 characters.
TELEMETRY_LIMIT = 127
# LP-1: the words of the command responses of LP-7, which the limit covers.
RESPONSE_WORDS = frozenset({"dcd", "cal", "resp"})
# LP-1 sets no limit on any other reply. The `# stats` line runs past 600 characters.
LINE_LIMIT = 2048

SEQUENCE_MODULUS = 256
BYTE_MAX = 255
ACCURACY_MAX = 3


class ProtocolError(ValueError):
    """A line is not one that the bench protocol defines."""


class Kind(Enum):
    """A telemetry tag and the sample it selects (LP-4, LP-6)."""

    E = "E"
    Q = "Q"
    R = "R"
    V = "V"
    U = "U"
    G = "G"


# LP-6: the fields after the four common ones, by tag. LP-4: E has none.
VALUE_NAMES: Mapping[Kind, tuple[str, ...]] = {
    Kind.E: (),
    Kind.Q: ("w", "i", "j", "k", "acc_rad"),
    Kind.R: ("w", "i", "j", "k"),
    Kind.V: ("x", "y", "z"),
    Kind.U: ("x", "y", "z", "bx", "by", "bz"),
    Kind.G: ("w", "i", "j", "k", "wx", "wy", "wz"),
}


@dataclass(frozen=True)
class Telemetry:
    """An E line (LP-4) or a payload line (LP-6).

    `time_us` is the event time in microseconds on the board's clock and
    `accuracy` is 0 (unreliable) to 3 (high). `values` follows `VALUE_NAMES`.
    """

    kind: Kind
    sensor: int
    seq: int
    time_us: int
    accuracy: int
    values: tuple[float, ...]


@dataclass(frozen=True)
class Reply:
    """A line that begins `# ` (LP-1, LP-3).

    `word` is the first word. `words` are the later words that are not
    `key=value` pairs, and `fields` holds the pairs.
    """

    word: str
    words: tuple[str, ...]
    fields: Mapping[str, str]


@dataclass(frozen=True)
class SaveDcdStatus:
    """The response to `savedcd` (LP-7): `# dcd status=<r0>`."""

    status: int


@dataclass(frozen=True)
class CalibrationStatus:
    """The response to `cal` or `calget` (LP-7): `# cal status=<r0> ...`."""

    status: int
    accel: int
    gyro: int
    mag: int
    planar: int
    table: int


@dataclass(frozen=True)
class RawResponse:
    """Any other command response (LP-7): `# resp cmd=<hex> seq=<n> ...`."""

    command: int
    request_seq: int
    response_seq: int
    result: bytes


Line = Telemetry | Reply | SaveDcdStatus | CalibrationStatus | RawResponse

_UNSIGNED = re.compile(r"[0-9]+")
# LP-6: five digits after the point, no exponent, no plus sign, and a magnitude
# below 1e9, so at most nine digits before the point. Otherwise `nan`.
_NUMBER = re.compile(r"-?[0-9]{1,9}\.[0-9]{5}|nan")
_RESPONSE_HEX = re.compile(r"[0-9a-f]{22}")
_TWO_HEX = re.compile(r"[0-9a-f]{2}")


def parse_line(text: str) -> Line:
    """Parse one line without its terminator."""
    if len(text) > LINE_LIMIT:
        raise ProtocolError(f"line is longer than {LINE_LIMIT} characters")
    if text.startswith("# "):
        return _parse_reply(text[2:], len(text))
    if len(text) > 1 and text[1] == "," and text[0] in {kind.value for kind in Kind}:
        return _parse_telemetry(Kind(text[0]), text)
    raise ProtocolError(f"not a protocol line: {text[:40]!r}")


def _parse_telemetry(kind: Kind, text: str) -> Telemetry:
    if len(text) > TELEMETRY_LIMIT:
        raise ProtocolError(
            f"telemetry line is longer than {TELEMETRY_LIMIT} characters (LP-1)",
        )
    fields = text.split(",")[1:]
    names = VALUE_NAMES[kind]
    if len(fields) != 4 + len(names):
        raise ProtocolError(
            f"{kind.value} line has {len(fields)} fields, expected {4 + len(names)}",
        )
    sensor = _unsigned(fields[0], "sensor", BYTE_MAX)
    seq = _unsigned(fields[1], "sequence", BYTE_MAX)
    time_us = _unsigned(fields[2], "time", None)
    accuracy = _unsigned(fields[3], "accuracy", ACCURACY_MAX)
    values = tuple(
        _number(field, name) for field, name in zip(fields[4:], names, strict=True)
    )
    return Telemetry(kind, sensor, seq, time_us, accuracy, values)


def _unsigned(field: str, name: str, maximum: int | None) -> int:
    if not _UNSIGNED.fullmatch(field):
        raise ProtocolError(f"{name} {field!r} is not a decimal number")
    value = int(field)
    if maximum is not None and value > maximum:
        raise ProtocolError(f"{name} {value} is above {maximum}")
    return value


def _number(field: str, name: str) -> float:
    if not _NUMBER.fullmatch(field):
        raise ProtocolError(
            f"{name} {field!r} is not a number with five decimals (LP-6)",
        )
    value = float(field)
    if value == 0.0 and field.startswith("-"):
        # LP-6: a negative value that rounds to zero prints without its sign.
        raise ProtocolError(f"{name} {field!r} is a zero with a sign (LP-6)")
    return value


def _parse_reply(body: str, length: int) -> Line:
    tokens = body.split(" ")
    if tokens[0] in RESPONSE_WORDS and length > TELEMETRY_LIMIT:
        raise ProtocolError(
            f"# {tokens[0]} line is longer than {TELEMETRY_LIMIT} characters (LP-1)",
        )
    if not body or "" in tokens:
        raise ProtocolError("reply has an empty word")
    word, rest = tokens[0], tokens[1:]
    words: list[str] = []
    fields: dict[str, str] = {}
    for token in rest:
        key, separator, value = token.partition("=")
        if not separator:
            words.append(token)
        elif not key:
            raise ProtocolError(f"reply field {token!r} has no name")
        elif key in fields:
            raise ProtocolError(f"reply field {key!r} appears twice")
        else:
            fields[key] = value
    if word == "dcd":
        return _parse_dcd(words, fields)
    if word == "cal":
        return _parse_calibration(words, fields)
    if word == "resp":
        return _parse_response(words, fields)
    return Reply(word, tuple(words), fields)


def _byte(fields: Mapping[str, str], key: str) -> int:
    return _unsigned(fields[key], key, BYTE_MAX)


def _expect_keys(
    word: str,
    words: list[str],
    fields: Mapping[str, str],
    keys: tuple[str, ...],
) -> None:
    if words or tuple(fields) != keys:
        raise ProtocolError(f"# {word} line must have exactly {', '.join(keys)} (LP-7)")


def _parse_dcd(words: list[str], fields: Mapping[str, str]) -> SaveDcdStatus:
    _expect_keys("dcd", words, fields, ("status",))
    return SaveDcdStatus(_byte(fields, "status"))


def _parse_calibration(
    words: list[str],
    fields: Mapping[str, str],
) -> CalibrationStatus:
    keys = ("status", "accel", "gyro", "mag", "planar", "table")
    _expect_keys("cal", words, fields, keys)
    return CalibrationStatus(*(_byte(fields, key) for key in keys))


def _parse_response(words: list[str], fields: Mapping[str, str]) -> RawResponse:
    _expect_keys("resp", words, fields, ("cmd", "seq", "rseq", "r"))
    if not _TWO_HEX.fullmatch(fields["cmd"]):
        raise ProtocolError("resp cmd must be two hex digits (LP-7)")
    if not _RESPONSE_HEX.fullmatch(fields["r"]):
        raise ProtocolError("resp r must be twenty-two hex digits (LP-7)")
    return RawResponse(
        int(fields["cmd"], 16),
        _byte(fields, "seq"),
        _byte(fields, "rseq"),
        bytes.fromhex(fields["r"]),
    )
