import socket
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Protocol

@dataclass(frozen=True)
class Board:
    serial: str
    product: str = ""
    ports: tuple[str, ...] = ()
    bootloader: bool = False
    drive: str | None = None
    listed: bool | None = None

class Host(Protocol):
    def clock(self) -> float: ...

class Proxy:
    def __init__(self, host: Host, path: Path, data: dict[str, Any]) -> None: ...
    def list(self) -> list[Board]: ...
    def allowed(self, serial: str) -> None: ...
    def open_serial(self, serial: str, baud: int, port: str) -> socket.socket: ...

def find_connection(start: str | Path) -> tuple[Path, dict[str, Any]] | None: ...
