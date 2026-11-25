# SPDX-License-Identifier: MIT
"""The four transfer rules of `gui/` (the embedded layout in PICO-GUI-PLAN).

The rules keep `gui/` movable: it can become its own repository by moving its
contents unchanged. Each rule is a function over text, tried first on a sample
that breaks it and then on the tree.
"""

import ast
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
OUTSIDE_PATH = re.compile(
    r"""(?:^|[\s"'`(=\[,])(?:
        \.\.[/\\]                                    # parent directory
        | /(?:home|workspaces|tmp|usr|etc|mnt|opt|var|root)/   # absolute path
        | [A-Za-z]:\\                                # Windows drive
        | (?:infra|examples|cmake)/                  # sibling of gui/
        | (?:CMakeLists\.txt|pico\.toml|house\.toml|dev\.py)\b
    )""",
    re.VERBOSE,
)
# `ruff.toml` names the House configuration it extends. The path changes when
# `gui/` moves, so this one key is allowed.
ALLOWED_OUTSIDE = {("ruff.toml", 'extend = "../infra/house/config/ruff.toml"')}


def outside_references(name: str, text: str) -> list[str]:
    return [
        line.strip()
        for line in text.splitlines()
        if OUTSIDE_PATH.search(line) and (name, line.strip()) not in ALLOWED_OUTSIDE
    ]


@pytest.mark.parametrize(
    "line",
    [
        'path = "../infra/x"',
        "open('/home/user/file')",
        "see /workspaces/plans/gui",
        r"C:\Users\someone",
        "read infra/guides/device.md",
        "include(cmake/house.cmake)",
        "uses examples/bench/bench.py",
        "run dev.py check",
    ],
)
def test_rule_2_the_checker_finds_a_path_outside(line: str) -> None:
    assert outside_references("x.md", line)


@pytest.mark.parametrize(
    "line",
    [
        "https://github.com/jamesfxley/pico-tooling.git",
        "src/pico_gui/shell.py",
        "tests/data/session.txt",
        "a/b",
        "forward port 8080",
    ],
)
def test_rule_2_the_checker_accepts_paths_inside(line: str) -> None:
    assert outside_references("x.md", line) == []


def test_rule_2_no_file_references_a_path_outside_gui() -> None:
    found = {
        str(path.relative_to(GUI)): hits
        for path in gui_files()
        if path != THIS_FILE
        and (hits := outside_references(path.name, path.read_text(encoding="utf-8")))
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
# `pico_tooling.device_proxy`.
ADAPTER_MODULES = {"device.py", "fakes.py"}
DEVICE_ACCESS = {"pico_tooling", "serial", "usb", "hid", "subprocess", "ctypes"}


def device_access(source: str) -> set[str]:
    return top_level(imported_modules(source)) & DEVICE_ACCESS


def test_rule_4_the_checker_finds_device_access() -> None:
    assert device_access("import serial") == {"serial"}
    assert device_access("from pico_tooling.device_proxy import Proxy") == {
        "pico_tooling",
    }
    assert device_access("import subprocess, os") == {"subprocess"}
    assert device_access("import socket") == set()


def test_rule_4_only_the_adapter_modules_import_the_proxy_client() -> None:
    found = {
        path.name
        for path in sources("pico_gui")
        if "pico_tooling" in device_access(path.read_text(encoding="utf-8"))
    }
    assert found == ADAPTER_MODULES


def test_rule_4_the_adapter_reaches_the_proxy_through_device_proxy() -> None:
    text = (SRC / "pico_gui" / "device.py").read_text(encoding="utf-8")
    assert "pico_tooling.device_proxy" in imported_modules(text)


def test_rule_4_no_other_module_opens_a_device_another_way() -> None:
    found = {}
    for package in ("pico_gui", "pico_gui_bno085"):
        for path in sources(package):
            if package == "pico_gui" and path.name in ADAPTER_MODULES:
                continue
            access = device_access(path.read_text(encoding="utf-8"))
            if access:
                found[f"{package}/{path.name}"] = access
    assert found == {}
