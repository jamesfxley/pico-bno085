# BNO085 GUI

A browser view of the BNO085 bench stream. The server runs in the dev
container. The browser runs on the host and reaches the server through VS Code
port forwarding.

This directory is a self-contained Python project with its own lock file. It
holds two packages:

- `pico_gui`, the generic core: transports, line framing, recording, replay and
  the shared shell. It imports no device package.
- `pico_gui_bno085`, the BNO085 device package: the protocol parser, the model,
  a panel and the launcher. It imports `pico_gui` only.

The parser follows requirements LP-1 to LP-7 of the library's technical
specification, section 8. The panel shows the connection state and the live
rate of each sensor. Richer panels are not built yet.

## Requirements

- Python 3.11 or newer, tested on 3.11 and 3.14.
- [uv](https://docs.astral.sh/uv/), which installs from `uv.lock`.
- The `pico_tooling` package, for a board. It is not yet in the lock (see
  [Dependencies](#dependencies)); until it is, put it on `PYTHONPATH`.

Install and run from this directory. `--locked` refuses a lock that does not
match `pyproject.toml`.

```sh
uv sync --locked
```

## Run

Replay a recording, with no board:

```sh
uv run --locked python -m pico_gui_bno085 --replay tests/data/session.txt
```

Show a board. The device proxy must be running on Windows (see the family's
device guide), and the board must be on its host allow-list.

```sh
uv run --locked python -m pico_gui_bno085 --serial BOARD-SERIAL
```

The board must run the bench firmware with `payload 1` set by whatever drives
it; the GUI only reads. Use `--device-port` when the board has more than one
port.

| Option | Meaning |
|---|---|
| `--serial S` | Open the board with serial number `S`. |
| `--replay FILE` | Replay a recording instead. Exactly one of the two is needed. |
| `--device-port P` | The board's serial port, if it has several. |
| `--record FILE` | Write each received line to `FILE`. |
| `--replay-rate N` | Lines a second of a replay. The default is 2000. |
| `--host ADDRESS` | Address to listen on. The default is `127.0.0.1`. |
| `--port N` | Port of the GUI server. The default is 8080. |

The exit status is 2 when the source cannot be opened, with a message on
standard error.

## Record and replay

`--record` writes the lines exactly as the device sent them, one per line,
with no timestamps. A recording needs no conversion to serve as a replay or a
test fixture. The files in `tests/data` are recordings.

## Port forwarding

The launcher prints the steps at start. VS Code does not forward the port by
itself. To open the GUI on the host:

1. Press F1, run **Forward a Port** and enter the port the launcher names. The
   Ports view also has a button that adds a port.
2. Open the forwarded address VS Code shows. Its local port can differ from the
   server's.

To stop forwarding, remove the port in the Ports view. VS Code's documentation
does not describe this step, so the exact menu entry is UNCONFIRMED.

Press Ctrl+C in the terminal to stop the server. The launcher then says the
forward can be removed. Use `--port` to choose another port when 8080 is taken.

## Checks

Run from this directory.

```sh
uv lock --check
ruff check --config ruff.toml .
ruff format --check .
uv run --locked mypy
uv run --locked pytest -W error
```

`ruff` must be the version that the House configuration pins. mypy runs in
strict mode over `src` and `tests`, with the `pico_tooling` interface declared
in `stubs`. Run pytest on the oldest and the newest supported Python:

```sh
uv run --locked --python 3.11 pytest -W error
uv run --locked --python 3.14 pytest -W error
```

`tests/test_transfer_rules.py` enforces four rules that keep this directory
movable to a repository of its own:

1. `pico_gui` imports nothing from any `pico_gui_<device>` package.
2. No file here refers to a path outside this directory. The one exception is
   the `extend` line of `ruff.toml`, which names the House configuration.
3. This directory has no build-system or firmware build dependency. It uses
   the device protocol through the specification's requirement IDs.
4. Device access goes only through the `pico_gui` transport adapter over
   `pico_tooling.device_proxy`.

## Dependencies

The runtime dependency is NiceGUI, pinned exactly. pytest and mypy are
development dependencies. `uv.lock` records every package with its hash,
including transitive ones.

`pico_tooling` is meant to be a git dependency on the family's tag `v0.2.2`.
That tag does not build as a package: its project file has no build system and
its flat layout holds several top-level directories, so `uv lock` fails. It
stays out of `pyproject.toml` until the tag installs. `stubs/pico_tooling`
declares the part of its interface that `pico_gui` uses, so mypy checks the
adapter without the package.

Setting `PYTHONWARNDEFAULTENCODING=1` for the test run, which PY-02.5 uses to
find text I/O without an encoding, fails at import: NiceGUI opens a file
without naming its encoding. Review checks this package's text I/O instead.
