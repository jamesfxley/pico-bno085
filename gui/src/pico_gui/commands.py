# SPDX-License-Identifier: MIT
"""Validate one command line before it reaches a transport (LP-1)."""

# The bench keeps the first 64 characters of a line, runs them as the command
# and drops the rest (LP-1).
COMMAND_LIMIT = 64

_PRINTABLE = frozenset(chr(code) for code in range(0x20, 0x7F))

__all__ = ("COMMAND_LIMIT", "CommandError", "command_bytes")


class CommandError(ValueError):
    """A command line the protocol refuses; nothing was written."""


def command_bytes(text: str) -> bytes:
    """Return the bytes of `text` terminated with a line feed.

    A longer line than `COMMAND_LIMIT` would run on the device as a shorter
    command than the one shown, so it is refused here. Every character must
    be printable ASCII, because a line feed or carriage return inside would
    end the command early and any other control character has no meaning in
    the protocol.
    """
    if not text:
        raise CommandError("a command line is empty")
    if len(text) > COMMAND_LIMIT:
        raise CommandError(
            f"the command line is longer than {COMMAND_LIMIT} characters",
        )
    for index, char in enumerate(text):
        if char not in _PRINTABLE:
            raise CommandError(
                f"the character at position {index + 1} is not printable ASCII",
            )
    return (text + "\n").encode("ascii")
