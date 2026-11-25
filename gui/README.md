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
- The `pico_tooling` package, for every run and every test run. It is not yet
  in the lock (see [Dependencies](#dependencies)); until it is, put a
  pico-tooling checkout on `PYTHONPATH`.

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
| `--device-port P` | The board's serial port, if it has several. Not with `--replay`. |
| `--record FILE` | Write each received line to `FILE`. |
| `--replay-rate N` | Lines a second of a replay, above zero. The default is 2000. |
| `--host ADDRESS` | Address to listen on. The default is `127.0.0.1`. |
| `--port N` | Port of the GUI server. The default is 8080. |

The exit status is 2 when the source cannot be opened, with a message on
standard error.

## Record and replay

`--record` writes the lines the session received, one per line, with no
timestamps, and flushes each line. The lines are as the framer returns them: an
empty line is absent, an overlong line is absent, a CR LF pair is one line feed
and a byte outside ASCII is written as `?`. A recording needs no conversion to
serve as a replay or a test fixture. The files in `tests/data` are recordings;
`tests/data/README.md` records where each came from.

A recording that cannot be written, for instance on a full disk, ends the
session with the reason shown in the status.

## Port forwarding

VS Code does not forward the port by itself. Once the server has bound its
port, the launcher prints these steps. To open the GUI on the host:

1. Run **Forward a Port** from the Command Palette (F1) and enter the port
   the launcher names.
2. Open `http://localhost:<port>` in the browser. VS Code may report a
   different local port; if it does, use that one.

A temporary forward lasts for the duration of the VS Code session. VS Code's
documentation does not describe stopping a forward before the session ends.

The steps follow the Dev Containers documentation
([forwarding or publishing a port](https://code.visualstudio.com/docs/devcontainers/containers#_forwarding-or-publishing-a-port)).

Press Ctrl+C in the terminal to stop the server. When the port cannot be
bound, the launcher prints no steps, says so on standard error and exits with
a non-zero status. Use `--port` to choose another port when 8080 is taken.

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
3. This directory depends on no firmware build or C++ build tool. It uses
   the device protocol through the specification's requirement IDs.
4. Device access goes only through the `pico_gui` transport adapter over
   `pico_tooling.device_proxy`.

## Dependencies

The runtime dependency is NiceGUI, pinned exactly. pytest and mypy are
development dependencies. `uv.lock` records every package with its hash,
including transitive ones.

`pico_tooling` becomes a locked dependency once it is installable as a
package. Until then it is not in `pyproject.toml` or `uv.lock`, and
`pico_tooling` must be on `PYTHONPATH`. `stubs/pico_tooling` declares the part
of its interface that `pico_gui` uses, so mypy checks the adapter without the
package; the stubs go when the package is installed.

Setting `PYTHONWARNDEFAULTENCODING=1` for the test run, which PY-02.5 uses to
find text I/O without an encoding, fails at import: NiceGUI opens a file
without naming its encoding. Review checks this package's text I/O instead.
