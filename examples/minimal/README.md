# minimal

The smallest thing that streams: reset the hub, enable the rotation vector at
50 Hz, print the quaternion.

```sh
python3 dev.py build --tree firmware
python3 -m infra.pico_tooling device flash --serial "$BOARD_SERIAL" build-firmware/examples/minimal/pico_bno085_minimal.uf2
python3 -m infra.pico_tooling device console --serial "$BOARD_SERIAL"
```

Device actions need specific authorization; `BOARD_SERIAL` names that board.
The [pinned device guide](../../infra/guides/device.md) owns proxy setup.

Wiring is [docs/usage-guide.md](../../docs/usage-guide.md). This is the I2C build; for
SPI, construct a `SpiHalTransport` instead — the rest is identical, which is
the point of the transport seam.

## What it shows

The desired sensor is recorded *before* the session is ready and applied when
it gets there, and re-applied after any hub reset, so the loop only has to pump
`service()`. That is the driver's job, not the caller's.

Every result is checked. `enable_sensor` returning `DEFERRED` here is normal —
the session is not READY yet — and is not an error; a failure would mean the
desired table is full, which one sensor cannot cause.

The loop also reads `faulted()`. `begin()` is non-blocking, so a hub that never
answers leaves an otherwise silent loop spinning; `startup_phase()` names the
phase whose deadline expired, which separates a hub that never spoke from one
that spoke and was mis-framed (TR-1).

## What it does not show

Nothing about batching, calibration, FRS, or the wake channel. For those, and
for anything measured, see [../bench](../bench).
