# SPDX-License-Identifier: MIT
"""The four transfer rules of `gui/` (the embedded layout in PICO-GUI-PLAN).

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
OUTSIDE_PATH = re.compile(
    START
    + r"""(?:
        /[A-Za-z_.][\w.\-]*                          # absolute path
        | ~[/\\]                                     # home directory
        | \$HOME\b | %USERPROFILE%                   # home directory variable
        | [A-Za-z]:\\                                # Windows drive
        | (?:infra|examples|cmake|docs|include|\.github|build[\w\-]*)/
        | src/pico_bno085\b
        | tests/(?:driver|bench|gui)\b
        | (?:CMakeLists\.txt|pico\.toml|house\.toml|dev\.py)\b
    )""",
    re.VERBOSE,
)
PARENT_TOKEN = re.compile(r"[^\s\"'`()=\[\],]*\.\.[/\\][^\s\"'`()=\[\],]*")
PARENTS_INDEX = re.compile(r"\.parents\[(\d+)\]")
# `ruff.toml` names the House configuration it extends. The path changes when
# `gui/` moves, so this one line of this one file is allowed.
ALLOWED_OUTSIDE = {("ruff.toml", 'extend = "../infra/house/config/ruff.toml"')}


def climbs_out(relative: str, token: str) -> bool:
    """Say whether `token`, written in the file `relative`, leaves `gui/`."""
    here = posixpath.dirname(relative)
    resolved = posixpath.normpath(posixpath.join(here, token.replace("\\", "/")))
    return resolved == ".." or resolved.startswith("../")


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
            or any(climbs_out(relative, t) for t in PARENT_TOKEN.findall(line))
            or any(int(n) >= depth for n in PARENTS_INDEX.findall(line))
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
        "read infra/guides/device.md",
        "include(cmake/house.cmake)",
        "uses examples/bench/bench.py",
        "see docs/technical-specification.md",
        "see include/pico_bno085/bno085.hpp",
        "see src/pico_bno085/driver.cpp",
        "run dev.py check",
        "Path(__file__).parents[3]",
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
    "ctypes",
    "socket",
    "termios",
    "pty",
    "fcntl",
    "importlib",
    "__import__",
}
ALLOWED_ACCESS = {
    "device.py": {"pico_tooling"},
    "fakes.py": {"pico_tooling"},
    "transport.py": {"socket"},
}


def device_access(source: str) -> set[str]:
    """Return the device-access names that `source` imports or calls.

    A call of `__import__` counts as a dynamic import of any module.
    """
    tree = ast.parse(source)
    calls = {
        node.func.id
        for node in ast.walk(tree)
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
    }
    return (top_level(imported_modules(source)) | calls) & DEVICE_ACCESS


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
        ("import os, json", set()),
    ],
)
def test_rule_4_the_checker_finds_device_access(
    source: str,
    expected: set[str],
) -> None:
    assert device_access(source) == expected


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
    found = {}
    for package in ("pico_gui", "pico_gui_bno085"):
        for path in sources(package):
            allowed = (
                ALLOWED_ACCESS.get(path.name, set()) if package == "pico_gui" else set()
            )
            extra = device_access(path.read_text(encoding="utf-8")) - allowed
            if extra:
                found[f"{package}/{path.name}"] = extra
    assert found == {}
