# SPDX-License-Identifier: MIT
"""The four transfer rules of `gui/` (the embedded layout rules).

The rules keep `gui/` movable: it can become its own repository by moving its
contents unchanged. Each rule is a function over text, tried first on a sample
that breaks it and then on the tree.
"""

import ast
import posixpath
import re
from collections.abc import Iterator
from pathlib import Path

import pytest

GUI = Path(__file__).resolve().parent.parent
SRC = GUI / "src"
THIS_FILE = Path(__file__).resolve()
SKIPPED_DIRECTORIES = {
    ".venv",
    ".mypy_cache",
    ".pytest_cache",
    ".ruff_cache",
    "__pycache__",
}
SKIPPED_FILES = {"uv.lock"}
DEVICE_PACKAGE = re.compile(r"pico_gui_[a-z0-9_]+")


def gui_files() -> Iterator[Path]:
    for path in sorted(GUI.rglob("*")):
        relative = path.relative_to(GUI)
        if (
            path.is_file()
            and not SKIPPED_DIRECTORIES & set(relative.parts)
            and relative.name not in SKIPPED_FILES
        ):
            yield path


def sources(package: str) -> Iterator[Path]:
    yield from sorted((SRC / package).rglob("*.py"))


def imported_modules(source: str) -> set[str]:
    """Return the top-level and dotted names that `source` imports."""
    names: set[str] = set()
    for node in ast.walk(ast.parse(source)):
        if isinstance(node, ast.Import):
            names.update(alias.name for alias in node.names)
        elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
            names.add(node.module)
    return names


def top_level(names: set[str]) -> set[str]:
    return {name.split(".")[0] for name in names}


# Rule 1: `pico_gui` imports nothing from any `pico_gui_<device>` package.
def device_imports(source: str) -> set[str]:
    return {
        name
        for name in top_level(imported_modules(source))
        if DEVICE_PACKAGE.fullmatch(name)
    }


def test_rule_1_the_checker_finds_a_device_import() -> None:
    assert device_imports("from pico_gui_bno085 import panel\nimport os") == {
        "pico_gui_bno085",
    }
    assert device_imports("import pico_gui_drv8833.model") == {"pico_gui_drv8833"}
    assert device_imports("from pico_gui import shell\nimport pico_tooling") == set()


def test_rule_1_the_core_imports_no_device_package() -> None:
    found = {
        path.name: device_imports(path.read_text(encoding="utf-8"))
        for path in sources("pico_gui")
    }
    assert {name: mods for name, mods in found.items() if mods} == {}


def test_rule_1_a_device_package_imports_the_core_and_itself_only() -> None:
    allowed = {"pico_gui", "pico_gui_bno085"}
    for path in sources("pico_gui_bno085"):
        own = {
            name
            for name in top_level(imported_modules(path.read_text(encoding="utf-8")))
            if name.startswith("pico_gui")
        }
        assert own <= allowed, path.name


# Rule 2: no file in `gui/` references a path outside `gui/`.
START = r"(?:^|[\s\"'`(=\[,])"
# A path that is outside `gui/` wherever the file sits: absolute, in a home
# directory, on a Windows drive, or a sibling of `gui/` in the repository.
ROOT_DIRECTORIES = (
    "home|workspaces|tmp|dev|proc|sys|usr|etc|var|opt|mnt|root|bin|sbin|lib|run|srv"
    "|media"
)
# A sibling directory counts only when a file or a deeper directory follows, so
# that prose such as "include/exclude" or "build/" does not match.
SIBLING_DIRECTORIES = r"(?:infra|examples|cmake|docs|include|\.github|build[\w\-]*)"
OUTSIDE_PATH = re.compile(
    START
    + rf"""(?:
        /(?:{ROOT_DIRECTORIES})/                    # known root, a segment after it
        | ~[/\\]                                     # home directory
        | \$HOME\b | %USERPROFILE%                   # home directory variable
        | [A-Za-z]:[/\\]                             # Windows drive, either slash
        | {SIBLING_DIRECTORIES}/[\w\-]+(?:/|\.\w)     # sibling directory, deeper
        | src/pico_bno085\b
        | tests/(?:driver|bench|gui)/
        | (?:CMakeLists\.txt|pico\.toml|house\.toml|dev\.py)\b
    )""",
    re.VERBOSE,
)
PARENT_TOKEN = re.compile(r"[^\s\"'`()=\[\],]*\.\.[/\\][^\s\"'`()=\[\],]*")
PARENTS_INDEX = re.compile(r"\.parents\[(\d+)\]")
PARENT_CHAIN = re.compile(r"(?:\.parent(?!\w))+")
CWD_PARENT = re.compile(r"cwd\(\)\.parent(?!\w)")
# A home directory reached through a call rather than a path, and a step
# out of the working directory that names no path.
HOME_CALL = re.compile(r"Path\.home\b|\.home\(\)|\bexpanduser\b")
CHDIR_PARENT = re.compile(r"chdir\s*\(\s*['\"]\.\.['\"]")
# `ruff.toml` names the House configuration it extends. The path changes when
# `gui/` moves, so this one line of this one file is allowed.
ALLOWED_OUTSIDE = {("ruff.toml", 'extend = "../infra/house/config/ruff.toml"')}


def climbs_out(relative: str, token: str) -> bool:
    """Say whether `token`, written in the file `relative`, leaves `gui/`."""
    here = posixpath.dirname(relative)
    resolved = posixpath.normpath(posixpath.join(here, token.replace("\\", "/")))
    return resolved == ".." or resolved.startswith("../")


def parent_chain_leaves(line: str, depth: int) -> bool:
    """Say whether a `.parent.parent...` chain on `__file__` leaves `gui/`.

    The first `.parent` is the file's directory; `depth` of them reach `gui/`.
    """
    return any(chain.count(".parent") > depth for chain in PARENT_CHAIN.findall(line))


def outside_references(relative: str, text: str) -> list[str]:
    """Return the lines of `text` that name a path outside `gui/`.

    `relative` is the file's path below `gui/`, with forward slashes.
    """
    depth = len(relative.split("/"))
    found = []
    for raw in text.splitlines():
        line = raw.strip()
        if (relative, line) in ALLOWED_OUTSIDE:
            continue
        if (
            OUTSIDE_PATH.search(line)
            or HOME_CALL.search(line)
            or CHDIR_PARENT.search(line)
            or any(climbs_out(relative, t) for t in PARENT_TOKEN.findall(line))
            or any(int(n) >= depth for n in PARENTS_INDEX.findall(line))
            or CWD_PARENT.search(line)
            or ("__file__" in line and parent_chain_leaves(line, depth))
        ):
            found.append(line)
    return found


@pytest.mark.parametrize(
    "line",
    [
        'path = "../infra/x"',
        "open('/home/user/file')",
        "see /workspaces/plans/gui",
        "cat /proc/cpuinfo",
        "read /dev/ttyACM0",
        "ls ~/notes",
        "echo $HOME",
        r"C:\Users\someone",
        'path = "C:/Users/someone"',
        "read infra/guides/device.md",
        "include(cmake/house.cmake)",
        "uses examples/bench/bench.py",
        "see docs/technical-specification.md",
        "see include/pico_bno085/bno085.hpp",
        "see src/pico_bno085/driver.cpp",
        "run dev.py check",
        "Path(__file__).parents[3]",
        "open('/tmp/x')",
        "see build/output/log.txt",
        "see docs/index.md",
        "uses tests/gui/helper",
        "Path.cwd().parent / 'infra'",
        "root = Path.cwd().parent",
        "p = Path.home()",
        "os.path.expanduser('~')",
        "p.expanduser()",
        "os.chdir('..')",
    ],
)
def test_rule_2_the_checker_finds_a_path_outside(line: str) -> None:
    assert outside_references("x.md", line)


def test_rule_2_a_parent_path_is_resolved_from_the_file() -> None:
    assert outside_references("tests/core/x.py", '"../../../infra/x"')
    assert outside_references("tests/core/x.py", '"../../../x"')
    assert outside_references("tests/core/x.py", "../../..")
    assert outside_references("tests/x.py", "data = '../../x'")


@pytest.mark.parametrize(
    ("relative", "line"),
    [
        ("tests/x.py", "Path(__file__).parent.parent.parent"),
        ("tests/core/x.py", "Path(__file__).resolve().parent.parent.parent.parent"),
        ("x.py", "Path(__file__).parent.parent"),
    ],
)
def test_rule_2_a_parent_chain_is_resolved_from_the_file(
    relative: str,
    line: str,
) -> None:
    assert outside_references(relative, line)


@pytest.mark.parametrize(
    ("relative", "line"),
    [
        ("tests/core/x.py", '"../data/session.txt"'),
        ("tests/core/x.py", '"../../src/pico_gui"'),
        ("tests/core/x.py", "Path(__file__).parents[2]"),
        ("tests/core/x.py", "see ../core/y.py"),
        ("x.md", "https://github.com/jamesfxley/pico-tooling.git"),
        ("x.md", "src/pico_gui/shell.py"),
        ("x.md", "tests/data/session.txt"),
        ("x.md", "a/b"),
        ("x.md", "and/or"),
        ("x.md", "forward port 8080"),
        ("src/pico_gui/x.py", 'ui.page("/monitor")'),
        ("x.py", 'url = "/api/state"'),
        ("x.py", 'fetch("/_nicegui/x")'),
        ("x.md", "the page `/status` shows it"),
        ("x.md", "rate in samples /s"),
        ("x.md", "include/exclude lists"),
        ("x.md", "the docs/ and src/ split"),
        ("x.md", "see build/ output"),
        ("x.md", "and tests/gui helpers"),
        ("tests/core/x.py", "Path(__file__).resolve().parent.parent.parent"),
        ("tests/core/x.py", "Path(__file__).parent"),
        ("tests/x.py", "tmp_path.parent.parent.parent"),
        # A bare known root is a page route, not a path.
        ("src/pico_gui/x.py", 'ui.page("/run")'),
        ("x.md", "the page `/home` shows it"),
        ("x.py", 'ui.link("/var")'),
        ("x.py", 'route = "/root"'),
        ("x.py", 'ui.page("/lib")'),
        ("x.md", "the home view"),
        ("x.py", "os.chdir(subdir)"),
    ],
)
def test_rule_2_the_checker_accepts_paths_inside(relative: str, line: str) -> None:
    assert outside_references(relative, line) == []


def test_rule_2_only_the_house_extend_line_is_allowed() -> None:
    allowed = 'extend = "../infra/house/config/ruff.toml"'
    assert outside_references("ruff.toml", allowed) == []
    assert outside_references("ruff.toml", 'extend = "../infra/other.toml"')
    assert outside_references("x.md", allowed)


def test_rule_2_no_file_references_a_path_outside_gui() -> None:
    """The rule is a text heuristic; these bypasses remain, as known limits.

    - `os.path.dirname` chains, and a `Path(__file__)` whose `.parent`
      chain sits on a later line, because the check reads one line at a
      time;
    - `Path("..") / "x"` and `Path(".").resolve().parent`, which build a
      path without naming it;
    - `Path("/") / "home"`, `file:///home/x` and `os.environ["HOME"]`,
      which reach a home directory another way;
    - a page route whose first segment is a known root with a segment
      after it, such as `/dev/tools` or `/sys/status`: those route names
      are reserved, and a panel takes another.
    """
    found = {
        relative: hits
        for path in gui_files()
        if path != THIS_FILE
        and (
            hits := outside_references(
                relative := path.relative_to(GUI).as_posix(),
                path.read_text(encoding="utf-8"),
            )
        )
    }
    assert found == {}


# Rule 3: `gui/` has no CMake or firmware build dependency.
BUILD_TOKENS = re.compile(
    r"cmake|ninja|pico[-_ ]sdk|\.uf2\b|arm-none-eabi|scikit-build",
    re.IGNORECASE,
)


def test_rule_3_the_checker_finds_a_build_dependency() -> None:
    for text in ("include(CMake)", "ninja", "PICO_SDK_PATH", "app.uf2", "scikit-build"):
        assert BUILD_TOKENS.search(text), text


def test_rule_3_no_cmake_file_exists() -> None:
    names = {path.name for path in gui_files()}
    assert "CMakeLists.txt" not in names
    assert not {name for name in names if name.endswith(".cmake")}


def test_rule_3_no_file_names_a_build_tool_or_firmware_image() -> None:
    found = [
        str(path.relative_to(GUI))
        for path in gui_files()
        if path != THIS_FILE and BUILD_TOKENS.search(path.read_text(encoding="utf-8"))
    ]
    assert found == []


# Rule 4: device access goes only through the `pico_gui` transport adapter over
# `pico_tooling.device_proxy`. Each module of `pico_gui` may use only the
# names listed for it; every other module uses none of them.
DEVICE_ACCESS = {
    "pico_tooling",
    "serial",
    "usb",
    "hid",
    "subprocess",
    "multiprocessing",
    "ctypes",
    "socket",
    "termios",
    "pty",
    "fcntl",
    "importlib",
    "__import__",
    "builtins.__import__",
    "os.system",
    "os.popen",
    "os.exec*",
    "os.spawn*",
    "os.posix_spawn",
    "http.client",
    "urllib.request",
    # Not the whole of `asyncio`, which NiceGUI code uses for timers and tasks:
    # only the calls that open a connection or start a process.
    "asyncio.open_connection",
    "asyncio.open_unix_connection",
    "asyncio.streams",
    "asyncio.create_subprocess_exec",
    "asyncio.create_subprocess_shell",
}
# Methods of an event loop that open a connection, whatever the loop is called.
LOOP_CONNECTION_CALLS = {
    "create_connection": "asyncio.create_connection",
    "create_unix_connection": "asyncio.create_unix_connection",
    "create_datagram_endpoint": "asyncio.create_datagram_endpoint",
}
ALLOWED_ACCESS = {
    "device.py": {"pico_tooling"},
    "fakes.py": {"pico_tooling"},
    "transport.py": {"socket"},
}
# The calls that can open a device file by its path. Device access goes only
# through the adapter (this rule), so a literal `/dev/` path is a bypass.
DEVICE_FILE_OPENERS = {"open", "os.open", "io.FileIO"}
# The keyword each opener takes its path as. `os.open` takes its path
# positionally only, so it has none.
DEVICE_FILE_KEYWORDS = {"open": "file", "io.FileIO": "name"}


def dotted_name(node: ast.expr, aliases: dict[str, str]) -> str | None:
    """Return the dotted name of a name or attribute chain, aliases resolved."""
    parts: list[str] = []
    while isinstance(node, ast.Attribute):
        parts.append(node.attr)
        node = node.value
    if not isinstance(node, ast.Name):
        return None
    parts.append(aliases.get(node.id, node.id))
    return ".".join(reversed(parts))


def candidate_names(source: str) -> set[str]:
    """Return the dotted names that `source` imports or calls."""
    tree = ast.parse(source)
    aliases = import_aliases(tree)
    names: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                names.add(alias.name)
        elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
            names.add(node.module)
            for alias in node.names:
                names.add(f"{node.module}.{alias.name}")
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            called = dotted_name(node.func, aliases)
            if called is not None:
                names.add(called)
            if (
                isinstance(node.func, ast.Attribute)
                and node.func.attr in LOOP_CONNECTION_CALLS
                and not (called or "").startswith("socket.")
            ):
                names.add(LOOP_CONNECTION_CALLS[node.func.attr])
    return names


def device_access(source: str) -> set[str]:
    """Return the device-access names that `source` imports or calls.

    A name matches when it equals a listed name or lies below it, so
    `http.client.HTTPConnection` matches `http.client`. A listed name
    ending in `*` matches every name that begins with its stem, so one
    entry covers a family such as `os.execv` and `os.execlpe`. A call of
    `__import__` counts as a dynamic import of any module.
    """
    candidates = candidate_names(source)
    return {
        listed
        for listed in DEVICE_ACCESS
        for name in candidates
        if (
            (listed.endswith("*") and name.startswith(listed[:-1]))
            or name == listed
            or name.startswith(listed + ".")
        )
    } | {listed for listed in LOOP_CONNECTION_CALLS.values() if listed in candidates}


def import_aliases(tree: ast.AST) -> dict[str, str]:
    """Return the aliases of `tree`'s imports, as `dotted_name` reads them."""
    aliases: dict[str, str] = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.Import):
            for alias in node.names:
                aliases[alias.asname or alias.name.split(".")[0]] = (
                    alias.name if alias.asname else alias.name.split(".")[0]
                )
        elif isinstance(node, ast.ImportFrom) and node.module and not node.level:
            for alias in node.names:
                aliases[alias.asname or alias.name] = f"{node.module}.{alias.name}"
    return aliases


def _begins_device_path(argument: ast.expr) -> bool:
    """Say whether `argument` is a string or f-string beginning `/dev/`."""
    if isinstance(argument, ast.Constant):
        return isinstance(argument.value, str) and argument.value.startswith("/dev/")
    if not isinstance(argument, ast.JoinedStr) or not argument.values:
        return False
    head = argument.values[0]
    return (
        isinstance(head, ast.Constant)
        and isinstance(head.value, str)
        and head.value.startswith("/dev/")
    )


def opens_device_file(source: str) -> bool:
    """Say whether `source` opens a device file by a literal path.

    The call the check knows is `open`, `os.open` or `io.FileIO` whose
    path is a string or an f-string beginning `/dev/`, passed
    positionally or as the opener's path keyword. A path held in a name
    at run time is beyond a name check, as the rule's docstring records.
    """
    tree = ast.parse(source)
    aliases = import_aliases(tree)
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        called = dotted_name(node.func, aliases)
        if called not in DEVICE_FILE_OPENERS:
            continue
        arguments: list[ast.expr] = list(node.args)
        keyword = DEVICE_FILE_KEYWORDS.get(called)
        if keyword is not None:
            arguments.extend(
                word.value for word in node.keywords if word.arg == keyword
            )
        for argument in arguments:
            if _begins_device_path(argument):
                return True
    return False


@pytest.mark.parametrize(
    ("source", "expected"),
    [
        ("import serial", {"serial"}),
        ("from pico_tooling.device_proxy import Proxy", {"pico_tooling"}),
        ("import subprocess, os", {"subprocess"}),
        ("import socket", {"socket"}),
        ("from socket import create_connection", {"socket"}),
        ("import termios", {"termios"}),
        ("import pty", {"pty"}),
        ("import importlib", {"importlib"}),
        ("from importlib import import_module", {"importlib"}),
        ("module = __import__('pico_tooling')", {"__import__"}),
        ("import usb.core", {"usb"}),
        ("import hid", {"hid"}),
        ("import ctypes", {"ctypes"}),
        ("import fcntl", {"fcntl"}),
        ("import multiprocessing", {"multiprocessing"}),
        ("from multiprocessing import Process", {"multiprocessing"}),
        ("import http.client", {"http.client"}),
        ("import urllib.request", {"urllib.request"}),
        ("from urllib import request\nrequest.urlopen(u)", {"urllib.request"}),
        ("import os\nos.system('x')", {"os.system"}),
        ("import os\nos.popen('x')", {"os.popen"}),
        ("from os import system", {"os.system"}),
        ("import builtins\nbuiltins.__import__('serial')", {"builtins.__import__"}),
        (
            "import asyncio\nasync def f():\n    await asyncio.open_connection(h, p)",
            {"asyncio.open_connection"},
        ),
        (
            "import asyncio as aio\naio.open_connection(h, p)",
            {"asyncio.open_connection"},
        ),
        ("from asyncio import open_connection", {"asyncio.open_connection"}),
        (
            "import asyncio\nasyncio.create_subprocess_exec('x')",
            {"asyncio.create_subprocess_exec"},
        ),
        (
            "import asyncio\nasyncio.get_running_loop().create_connection(f, h)",
            {"asyncio.create_connection"},
        ),
        ("import asyncio\nloop.call_later(1, f)\nasyncio.sleep(1)", set()),
        ("import socket\nsocket.create_connection(a)", {"socket"}),
        ("import os, json", set()),
        ("import asyncio.streams", {"asyncio.streams"}),
        ("from asyncio import streams", {"asyncio.streams"}),
        (
            (
                "from asyncio.streams import open_connection as open_stream\n"
                "open_stream(h, p)"
            ),
            {"asyncio.streams"},
        ),
        ("import os\nos.execv(path, argv)", {"os.exec*"}),
        ("import os\nos.execlpe(path, 'x', 'y')", {"os.exec*"}),
        ("import os\nos.spawnv(mode, argv)", {"os.spawn*"}),
        ("import os\nos.spawnlp(mode, path, 'x')", {"os.spawn*"}),
        ("import os\nos.posix_spawn(path, argv, env)", {"os.posix_spawn"}),
        (
            "import asyncio\nasyncio.open_unix_connection(path)",
            {"asyncio.open_unix_connection"},
        ),
        (
            "import asyncio\nasyncio.create_subprocess_shell('x')",
            {"asyncio.create_subprocess_shell"},
        ),
        (
            "import asyncio\nasyncio.get_running_loop().create_unix_connection(f, p)",
            {"asyncio.create_unix_connection"},
        ),
        (
            "import asyncio\nasyncio.get_running_loop().create_datagram_endpoint(f)",
            {"asyncio.create_datagram_endpoint"},
        ),
    ],
)
def test_rule_4_the_checker_finds_device_access(
    source: str,
    expected: set[str],
) -> None:
    assert device_access(source) == expected


@pytest.mark.parametrize(
    "source",
    [
        'open("/dev/ttyACM0", "rb")',
        'os.open("/dev/tty", os.O_RDWR)',
        'io.FileIO("/dev/x")',
        'from io import FileIO\nFileIO("/dev/x")',
        'from os import open as osopen\nosopen("/dev/x", 0)',
        'open(file="/dev/ttyACM0")',
        'io.FileIO(name="/dev/x")',
        'open(f"/dev/{path}", "rb")',
        'os.open(f"/dev/{name}", 0)',
    ],
)
def test_rule_4_the_checker_finds_a_device_file_opened_by_path(source: str) -> None:
    assert opens_device_file(source)


@pytest.mark.parametrize(
    "source",
    [
        'open("/tmp/notes")',
        "open(path)",
        "os.open(path, 0)",
        'io.FileIO("recording.bin")',
        'open(file="/tmp/notes")',
        'open(mode="rb", file="/tmp/x")',
        'io.FileIO(name="recording.bin")',
        'open(f"/tmp/{name}")',
    ],
)
def test_rule_4_the_checker_leaves_other_files_alone(source: str) -> None:
    assert not opens_device_file(source)


def test_rule_4_only_the_adapter_modules_may_import_the_proxy_client() -> None:
    found = {
        path.name
        for path in sources("pico_gui")
        if "pico_tooling" in device_access(path.read_text(encoding="utf-8"))
    }
    assert found <= {"device.py", "fakes.py"}


def test_rule_4_the_adapter_reaches_the_proxy_through_device_proxy() -> None:
    text = (SRC / "pico_gui" / "device.py").read_text(encoding="utf-8")
    assert "pico_tooling.device_proxy" in imported_modules(text)


def test_rule_4_no_module_opens_a_device_another_way() -> None:
    """The check reads names, so it stays heuristic, with known limits.

    `asyncio.start_server`, `loop.sock_connect`, `os.fork`, `select`,
    `selectors`, `_socket` and the HTTP and WebSocket client libraries
    are not listed. The device-path check sees `open`, `os.open` and
    `io.FileIO` with a string or f-string path, positional or the
    opener's path keyword, so a path held in a name at run time is
    beyond it.
    """
    found = {}
    for package in ("pico_gui", "pico_gui_bno085"):
        for path in sources(package):
            allowed = (
                ALLOWED_ACCESS.get(path.name, set()) if package == "pico_gui" else set()
            )
            text = path.read_text(encoding="utf-8")
            extra = device_access(text) - allowed
            if extra:
                found[f"{package}/{path.name}"] = extra
            assert not opens_device_file(text), f"{package}/{path.name}"
    assert found == {}
