# BNO085 GUI

A browser view of the BNO085 bench stream that also drives the bench: it
enables sensors, tares and calibrates the hub, and records what it receives.
The server runs in the dev container. The browser runs on the host and
reaches the server through VS Code port forwarding.

This directory is a self-contained Python project with its own lock file. It
holds two packages:

- `pico_gui`, the generic core: transports, line framing, command lines,
  recording, replay and the shared shell. It imports no device package.
- `pico_gui_bno085`, the BNO085 device package: the protocol parser, the
  stream figures, the controller, the panels and the launcher. It imports
  `pico_gui` only.

The parser follows requirements LP-1 to LP-8 of the library's technical
specification, section 8.

## Requirements

- Python 3.11 or newer, tested on 3.11 and 3.14.
- [uv](https://docs.astral.sh/uv/), which installs from `uv.lock`.

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

Drive a board. The device proxy must be running on Windows (see the family's
device guide), and the board must be on its host allow-list.

```sh
uv run --locked python -m pico_gui_bno085 --serial BOARD-SERIAL --bus spi
```

| Option | Meaning |
|---|---|
| `--serial S` | Open the board with serial number `S`. |
| `--bus B` | The bus the hub sits on, `i2c` or `spi`. Required with `--serial`, and refused with `--replay`. |
| `--replay FILE` | Replay a recording instead. Exactly one of the two sources is needed. |
| `--device-port P` | The board's serial port, if it has several. Not with `--replay`. |
| `--record FILE` | Write each received line to a new file. A file that exists is refused, the `--replay` file included, with a message and exit status 2 before anything is opened; a recording is never overwritten. |
| `--record-dir DIR` | Directory the record button writes generated files into. A generated name that exists is skipped for another; no file is overwritten. |
| `--replay-rate N` | Lines a second of a replay, above zero. The default is 2000. |
| `--host ADDRESS` | Address to listen on. The default is `127.0.0.1`. |
| `--port N` | Port of the GUI server. The default is 8080. |

`--record` and `--record-dir` are two ways to write the same lines, so the
launcher refuses to take both. The exit status is 2 when the source cannot be
opened, with a message on standard error.

## The command path

On a board, the launcher sends `bus <i2c|spi>` and waits for its reply, then
`payload 1` and waits again (LP-2, LP-3). A command the panel issues is
queued and sent one at a time, each matched to its reply; a refusal and a
missing reply within two seconds become the line under the header, never an
exception. The two-second wait is a chosen value: the specification states
no duration for a bus to come up or for a command to be answered. `tare`,
`cal` and `calget` wait for a `# ready` line first, because the driver
refuses them until the session is READY.

A command whose reply does not come within its wait is kept on a list of
suspects, oldest first, because the bench answers every command in order
and a late answer can only sit ahead of the reply to the command sent after
it. Only a line that can be a reply — one beginning `# ok`, `# err`,
`# bus`, `# payload` or `# run` — is tested against the suspects;
telemetry and the asynchronous lines leave the list as it is. A suspect
reply is dropped, the line under the header says which timed-out command
it may answer, and the command in flight keeps its turn for its own reply.
A line of another form means the oldest suspect's reply was lost; the line
is tested against the next suspect, and when none matches it serves the
command in flight. A command whose reply was a dropped one is not made a
suspect when its own wait then passes: its outcome is unknown and the line
under the header says so. That reading has a limit the order of replies
cannot resolve: a dropped line may have been the command's own reply
rather than the earlier suspect's late answer, and the controller reads
it as the suspect's answer, so if every reply was only slow, the marked
command's own late reply is applied to the command sent after it — two
consecutive slow replies bring that about. Command A times out, its late
reply is dropped as a suspect's and marks B, B times out as unknown, and
B's own late reply is then applied to the command C sent after it. Each
suspect leaves the list once its wait has passed by more than one further
wait, two waits past its own, because the command sent after a timeout
answers at least one wait later.

When the bus never answers, or a command cannot be written to the stream,
the Retry connect control appears; it asks for the whole connect sequence
again, clears the suspects, and a late `# bus` answer resumes the sequence
on its own. The board is expected to hold every enabled sensor as desired
across a retry and re-enable it; that it does is UNCONFIRMED, the
specification stating the re-enabling only for a hub reset, so the panel
keeps its knowledge as the session's best account.

The bench reads commands while a `run` is printing, so a queued command goes
out as soon as no other command is in flight. While any sensor is enabled the
controller sends `run 1000` again after each `# done`. The one-second run
length bounds how long telemetry keeps flowing after the last sensor is
disabled, and how long the stream stays silent after a lost `# done` line,
the run length plus a two-second grace, before the controller sends another
run.

Every command line is validated before it is sent: ASCII, at most 64
characters, ending in one line feed (LP-1). An interval or a duration the
bench's parser would refuse, one above the unsigned 32-bit range, is refused
on the host and noted below the tables.

A recording has no board to command. In a replay every command control is
disabled and the line under the header says why.

## The panels

The page has two columns. The live column, on the left, holds the
connection bar, the command-path line with Retry, the two status lines, the
board orientation view beside the Euler readouts, and the three motion plots
in one row below them; it never changes with the controls. The controls sit
in one tab panel on the right, one tab visible at a time: Sensors (the
default), Health, Calibration, and Recording and Replay when the launcher
gave `--record-dir` or `--replay`. In a window narrower than Quasar's
medium breakpoint (1024 px) the tab panel falls below the live column, and
the plots and the orientation view wrap onto separate lines when a row is
too narrow for them. The layout uses NiceGUI's own elements and Quasar
classes only.

The connection bar names the source and shows the session state, its line
count and any line dropped for being too long. The line under the header
shows the command path: the phase, the reply being waited for, the queue
depth and the last refusal, timeout or restart.

The Sensors tab shows a table that lists each sensor that has reported, with the
state the session commanded, the commanded rate beside the delivered rate, the
sample count of the window and the latest accuracy as its LP-4 name. The G line
shows no accuracy, because its report carries a placeholder (1000-3625
§6.5.44.2). Below the table, the sensor select offers the thirteen sensors of
the LP-8 table with their maximum rates, naming the two rates the specification
marks unconfirmed; the interval input takes the report interval in microseconds
and Enable sends `cfg <id_hex> <us>`, Disable sends `off <id_hex>`. An interval
of zero is refused: a report interval of zero turns the sensor off (1000-3625
§5.4.1), which `cfg` would do while the bench keeps the sensor's desired entry,
so the panel disables with `off`.

The board orientation panel shows a board turning in a three-dimensional
view, built from the same stream and sending no command of its own. The
selector picks one of the three rotation sources: the rotation vector
(`05`), referenced to magnetic north and gravity; the game rotation
vector (`08`), referenced to gravity alone and carrying no heading
reference; or the gyro-integrated rotation vector (`2A`), whose reference
frame the specification leaves UNCONFIRMED (LP-6). Selecting a source
sends no command: the view takes that source's latest sample at each
page refresh, and a source that has printed nothing leaves a note naming
the sensor to enable. The view draws the device frame of LP-6 — X to the
right, Y along the face, Z out of the front face — on the scene's own
X, Y and Z. LP-6 does not confirm the direction of the rotation the
quaternion expresses or the axes of its reference frame, so the board may
show the inverse orientation.

The Euler readouts show the selected source's angles in degrees as
intrinsic Z-Y′-X″ angles: yaw about Z, then pitch about the new Y, then
roll about the newest X, with yaw and roll in (−180, 180] and pitch in
[−90, 90]. At either end of the pitch range, where the yaw and the roll
of one rotation are a single observable, roll reports zero and yaw takes
the whole of it. The game rotation vector's yaw has no heading reference.

The motion plots show the calibrated accelerometer (`01`) in m/s², the
gyroscope (`02`) in rad/s and the magnetometer (`03`) in µT, each axis against
event time in seconds over the latest five seconds of that sensor's own clock
(LP-6). Only the sensor's `V` lines carry those values, so a recording made
with the payload off leaves them out, and a restart that steps the event clock
back starts a fresh window. The model keeps five seconds at the
accelerometer's LP-8 maximum of 500 Hz, the fastest plotted sensor; the
retained samples cost 0.8 to 1.1 MiB per sensor that reports, by the width of
its line, so about 11 MiB when thirteen sensors report `V` lines and at most
about 15 MiB when all thirteen report `G` lines. Each series is thinned to at
most 300 points, keeping the newest, and a chart redraws only when its sensor
has a new sample, at most 10 times a second. A sensor that has printed no `V`
line, either nothing or only `E` lines, shows no samples and its identifier.
Only a backward step of the event time of 400 µs or more starts a fresh window
or counts as a fault; PR-3 allows smaller steps, which stay inversions. An
unflagged step of that size is a reconstruction fault under PR-3: the health
table's clock-faults column adds one, the window starts afresh, and the count
outlives the window. A step at the first sample after a `# ready` or `# run`
line is taken as a restart and not counted; this flag is the GUI's rule, not
PR-3's, so it hides a real backward step that happens to fall there, and a
recording that joins two passes without one of those lines between them counts
one fault per sensor at the join. A replay Restart reads the recording into a
new model, so its second pass starts with empty figures. The board view, the
readouts and the tables follow the page refresh, at most 30 times a second; a
table, the board's rotation and every text are sent to the browser only when
their content changed since the last refresh.

The Health tab shows the stream health table: per sensor over a rolling
two-second window of event time: the delivered rate, the missing and the
repeated report counts, the timestamp inversions, the worst backward step, the
clock faults and the 99th-percentile jitter as a fraction of the nominal
interval. The rate comes from the event times the lines carry, so a burst of
lines in one USB chunk leaves it unchanged. The nominal interval is the
commanded one when this session sent `cfg` for that sensor, and otherwise the
median observed interval; the table's last column states which basis was used.
The G line's sequence is a placeholder, so it is left out of the gap counts, and
nothing is compared across a `# ready` or `# run` boundary, where the hub's
counters may have reset.

The Calibration tab shows the last `# cal` response decoded
per LP-7, with the status and each routine's state, and the last command
response line as it was received. The tare controls take the axes, 7 for
X, Y and Z or 4 for Z alone, and the basis, the rotation vector the tare is
taken against: rotation, game rotation, geomagnetic rotation,
gyro-integrated rotation, ARVR-stabilised rotation or ARVR-stabilised game
rotation (1000-3625 §6.4.3.1). The tare response is shown as received; the
layout of its bytes is UNCONFIRMED (LP-7, FW-2). Calibrate sends
`cal <accel> <gyro> <mag>` from the three switches and also turns the
planar-accelerometer and on-table routines off (1000-3625 §6.4.6.1); Read
calibration state sends `calget` and the response is decoded like any
`# cal` line.

Commands the panels do not offer: `info`, which prints the constants the bench
harness needs and has no use in a panel, `stat` and `heap`, read-only reports a
terminal session serves as well, `flush`, which asks a batching sensor to
deliver at once, and `tarepersist`, `savedcd`, `pdcd`, `cleardcd`, `reset`,
`boot` and `trace`, which write the hub's flash or disrupt the session. The
panels leave all of them to a terminal on the bench.

The Recording tab appears when the launcher was given `--record-dir`. Start
opens a file whose name is generated from the time, so no name is typed into the
browser, and shows the file being written; while a recording runs, Start is
disabled and a direct call to it refuses with the panel's message. Stop closes
it and waits for a write in progress, so a stalled disk delays it. A recording
that cannot be written, for instance on a full disk, is noted in the panel and
the session keeps reading.

The Replay tab appears for `--replay`. It shows the fraction of the recording
the pass has read, Hold pauses the pass where it stands, Resume continues it,
and Restart stops the pass and reads the recording again into a fresh model;
until new samples arrive the charts and the board keep their last drawing,
although each note reads that there are no samples. A recording runs across
passes: a pass that ends leaves the recording attached, so Restart keeps writing
the same file.

## Record and replay

`--record` writes the lines the session received, one per line, with no
timestamps, and flushes each line. The lines are as the framer returns them: an
empty line is absent, an overlong line is absent, a CR LF pair is one line feed
and a byte outside ASCII is written as `?`. A recording needs no conversion to
serve as a replay or a test fixture. `tests/data/README.md` records where each
file in `tests/data` came from.

A recording that cannot be written, for instance on a full disk, ends the
session with the reason shown in the status. A recording started from the
record button does not end the session; it stops and the panel says why.

## Port forwarding

VS Code does not forward the port by itself. Once the server has bound its
port, the launcher prints these steps. To open the GUI on the host:

1. Run **Forward a Port** from the Command Palette (F1) and enter the port
   the launcher names. A notification will tell you the localhost port you
   should use.
2. Open the localhost port the notification names in the browser, for
   example `http://localhost:4123`.

A temporary forward lasts for the duration of the VS Code session. VS Code's
documentation does not describe stopping a forward before the session ends.

The steps follow the Dev Containers documentation
([forwarding or publishing a port](https://code.visualstudio.com/docs/devcontainers/containers#_forwarding-or-publishing-a-port)).

Press Ctrl+C in the terminal to stop the server. When the port cannot be
bound, the launcher prints no steps, says so on standard error and exits with a
non-zero status. Use `--port` to choose another port when 8080 is taken.

## Checks

Run from this directory.

```sh
uv lock --check
ruff check --config ruff.toml .
ruff format --check .
uv run --locked mypy
uv run --locked --python 3.14 pytest -q -W error
uv run --locked --python 3.11 pytest -q -W error
```

`ruff` must be the version that the House configuration pins. mypy runs in
strict mode over `src` and `tests`, with the `pico_tooling` interface declared
in `stubs`.

`tests/test_transfer_rules.py` enforces four rules that keep this directory
movable to a repository of its own:

1. `pico_gui` imports nothing from any `pico_gui_<device>` package.
2. No file here refers to a path outside this directory. The one exception is
   the `extend` line of `ruff.toml`, which names the House configuration.
3. This directory depends on no firmware build or C++ build tool. It uses
   the device protocol through the specification's requirement IDs.
4. Device access goes only through the `pico_gui` transport adapter over
   `pico_tooling.device_proxy`.

The user-interface tests run the page in NiceGUI's headless user simulation,
which drives the page builders the same version of the framework serves in a
browser. `tests/bno085/test_parity.py` feeds the recordings of
`tests/data` through the model and checks the figures against the bench
harness's own functions on the same lines; the expected figures are committed
with the test.

## Dependencies

The runtime dependencies are NiceGUI, pinned exactly, and pico-tooling, a git
dependency on its `v0.2.3` tag. pytest, mypy and pytest-asyncio are
development dependencies. `uv.lock` records each registry package with its
hash and the git dependency by the commit its tag names, including
transitive packages.

pico-tooling ships no `py.typed`, so strict mypy treats the installed package
as untyped. `stubs/pico_tooling` declares the part of its interface that
`pico_gui` uses, so mypy checks the adapter against a typed interface; the
stubs go when the package ships a `py.typed`.

Setting `PYTHONWARNDEFAULTENCODING=1` for the test run, which PY-02.5 uses to
find text I/O without an encoding, fails at import: NiceGUI opens a file
without naming its encoding. Review checks this package's text I/O instead.
