# bench

The on-device validation harness. Brings the hub up over either bus and answers
a line protocol that `bench.py` drives to run the checklist.

The firmware only measures and reports; every threshold lives in `bench.py`, so
the device stays a dumb instrument and changing a gate needs no reflash.

```sh
python3 dev.py build --tree firmware          # from the repository root
python3 examples/bench/bench.py --port "$RUN/bno-tty" --bus spi
```

Follow [the pinned family device guide](../../infra/guides/device.md) for
flashing and console access. The bench image is
`build-firmware/examples/bench/pico_bno085_bench.uf2`. Close other clients of
the serial port before flashing or benchmarking. These are device actions,
not smoke checks; a device action needs specific authorization.

For the proxy route, set `BOARD_SERIAL` to the authorized serial and `RUN` to
a disposable folder in both terminals. In one terminal, from the repository:

```sh
socat PTY,link="$RUN/bno-tty",raw,echo=0 EXEC:"python3 -m infra.pico_tooling device console --serial $BOARD_SERIAL"
```

Run the bench command above in the other terminal once the PTY exists. Stop
the bridge with Ctrl+C after the bench finishes. `--port` opens the path given;
omitting it prints this route instead of searching for a device.

One image covers both buses: `bus i2c|spi` selects at run time. `bench.py
--bus` is required and chooses i2c or spi for the session. A case whose
requirement names a bus selects it itself, whatever `--bus` says: `b-rv400` and
`b-girv` run on SPI, `b-i2c100` on I2C, and `b-parity` on each in turn. The
session's bus is selected again after each of them, and every result records
the bus acknowledged by the firmware (both replies for b-parity).

## Line protocol

One command per line. Replies begin `#`; telemetry lines begin `E,`.

| Command | Does |
|---|---|
| `bus i2c\|spi` | Bring up a bus. Switching re-straps with the hub held in reset. |
| `cfg <id_hex> <us>` | Enable or reconfigure a sensor at a report interval |
| `off <id_hex>` | Disable a sensor |
| `flush <id_hex>` | Make a batching sensor deliver now |
| `run <ms>` | Stream events, then a `# stats` summary and `# done` |
| `stat` | A `# stats` line now |
| `heap` | Post-init heap growth (AR-1) |
| `reset` | Restart the hub, using the executable-channel reset |
| `cleardcd` | Clear the calibration and reset |
| `trace 0\|1` | Dump the raw bytes of every transfer |
| `boot` | Re-run the handshake without reflashing |
| `info` | Constants the harness needs to read the output |

Telemetry is `E,<sensor>,<seq>,<event_us>,<accuracy>`. The stats line is
`key=value` pairs, and the session's counters are emitted by walking the set,
so a counter added in pico-sh2 appears here without an edit.

## The checklist

| Test | Gates |
|---|---|
| `b-boot` | Reset to READY |
| `b-rate` | A sensor at a requested rate; delivered rate and drops |
| `b-rv400` | Rotation vector at 400 Hz (PR-1) |
| `b-ts` | Timestamp ordering within the reconstruction bound (PR-3) |
| `b-rvphase` | Rotation vector and gyroscope at one rate over a separable interval; their phase is recorded, not gated (PR-3) |
| `b-i2c100` | Accel + gyro + mag concurrently at 100 Hz (PR-2) |
| `b-girv` | Gyro-integrated rotation vector at its gate rate (PR-4) |
| `b-lat` | Worst HINTN-to-read service gap (HR-5) |
| `b-heap` | Post-init heap growth is zero (AR-1) |
| `b-parity` | Both buses deliver the same stream (HR-4) |
| `b-reset` | Recovery and desired-sensor re-enable |
| `b-soak` | A long run with the error counters at zero (OR-1) |

`b-soak` takes an hour and is excluded from a default run; name it explicitly.
Promoted results and the measured figures are in
[baselines/README.md](baselines/README.md).

## When the board is not talking

`trace 1` shows the wire and `boot` retries the handshake. If the question is
whether the pads and jumpers work at all, check the power, straps and wiring
in [the usage guide](../../docs/usage-guide.md).

`b-rv400` enables the accelerometer at 50 Hz halfway through its 400 Hz SPI
rotation-vector stream. It keeps collecting events while the configuration
write is acknowledged, and requires zero sequence gaps and reassembly drops.

## Wire timing diagnostic

`trace 1` prints the capped transfer format above. `trace 2` prints each
successful read as one line, including all header and cargo bytes:

```text
# wire edge_us=123456 n=9: 09 00 03 00 fb 0a 00 00 00
```

`edge_us` is the transport's captured HINTN edge in microseconds; `n` is the
byte count. Errors are `# wire edge_us=0 err=<code>`. Writes retain the
`trace 1` format. `trace 0` disables either mode. The firmware walks fixed
storage and prints directly; full tracing costs serial bandwidth and service
time, so it is diagnostic evidence rather than an unperturbed timing run.

```text
./bench.py --port /tmp/bno-tty --bus spi --tests b-rvphase --wire --output-dir /tmp/bno-wire
```

Each invocation selects the bus and resets the hub. Isolation disables all
bench sensors, including gyro-integrated RV, before enabling gyro and RV.
`--wire` enables full tracing before sensor configuration, then disables it
after the phase window. It records feature grants, independently reconstructs
RV/gyro times, and pairs wire and driver events by sensor ID and sequence in
arrival order. The result detail names a `b-rvphase-wire-<UTC>.json` sidecar
under `--output-dir`, without requiring `--promote`. It contains full frames,
per-event agreement, wire-only nearest phase offsets and granted intervals.
Missing grants, unmatched events, decode errors and unsupported fragmented or
unknown cargoes are explicit; they do not establish agreement. Wire decoding
uses SH-2 1000-3625 §§6.5.5, 7.2 and BNO08X 1000-3927 §1.3.5.3's worked
example (the delay-sign discrepancy is noted in PR-3).

The diagnostic changes no PASS/FAIL rule. It runs only for `b-rvphase`; the
full release suite runs without full tracing.
