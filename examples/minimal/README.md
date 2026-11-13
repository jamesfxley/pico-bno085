# minimal

This I2C example resets the hub, enables rotation vectors at 50 Hz and prints
the quaternion.

```sh
python3 dev.py build --tree firmware
python3 -m infra.pico_tooling device flash --serial "$BOARD_SERIAL" build-firmware/examples/minimal/pico_bno085_minimal.uf2
python3 -m infra.pico_tooling device console --serial "$BOARD_SERIAL"
```

Device actions need specific authorisation; `BOARD_SERIAL` names that board.
The [pinned device guide](../../infra/guides/device.md) owns proxy setup.

Wiring is [docs/usage-guide.md](../../docs/usage-guide.md). This is the I2C
build; for SPI, the transport is `SpiHalTransport`. The rest of the example is
identical.

## Sensor recovery

The driver records the desired sensor before READY and sends the configuration
on reaching READY. It reapplies the configuration after reset, so the loop
only has to call `service()`.

Every result is checked. `enable_sensor` returning `DEFERRED` here is expected
because startup is incomplete. A full desired table would fail, but one sensor
cannot fill it.

The loop also reads `faulted()`. `begin()` is non-blocking, so a hub that
never answers leaves an otherwise silent loop spinning; `startup_phase()`
names the phase whose deadline expired. The [startup
guide](../../docs/usage-guide.md#flash-and-run) explains the fault phases.

## Other features

This example omits batching, calibration, FRS and the wake channel. The
[benchmark](../bench) provides the device checklist and transfer tracing.
