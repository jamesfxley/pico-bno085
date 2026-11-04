# Device baselines

The run record: one promoted run per profile, named
`pico2-v<major.minor>-<profile>.json`, plus the conditions each was taken under
and what the figures mean. `bench.py --promote` writes `bench-<timestamp>.json`
here, and promotion renames it.

This is where measured values live. The specification states the gates; it does
not state what a particular board did on a particular day.

## Run conditions

Record alongside each promoted run:

- Board and part (Pico 2 W; BNO085 or BNO086).
- Bus and rate. One firmware image serves both buses; which bus each case runs
  on is the harness's, described in [../README.md](../README.md), and each
  result records it.
- Wiring per [docs/usage-guide.md](../../../docs/usage-guide.md).
- SDK version, `PICO_BNO085_PROFILE`, and the pico-bno085 version the image
  was built from.

## Promotion

A release's baseline is its latest full run, one file per profile. A newer run
on the same release replaces it in place; earlier releases keep theirs. Rename
the promoted `bench-<timestamp>.json` to `pico2-v<major.minor>-<profile>.json`
— the timestamped name is the tool's output, not the baseline's name.

`b-soak` is excluded from a default run because it takes an hour. A release
baseline includes it, so name it explicitly. A baseline's session bus is SPI,
so select it explicitly with the required `--bus` argument:

```sh
./bench.py --port "$RUN/bno-tty" --bus spi --promote --tests b-boot,b-rate,b-rv400,b-ts,b-rvphase,b-i2c100,b-girv,b-lat,b-heap,b-parity,b-reset,b-soak
```

**Start a full run from a cold boot**, not from a board left running the
previous one. `b-girv` leaves the gyro-integrated rotation vector in the
driver's desired-sensor table, and the driver re-applies that table across a
reset — AR-3 working as specified, on the reset pico-sh2's SR-5 recognises —
so a second run back-to-back starts with
GIRV already enabled. 1000-3625 §5.4.1 says GIRV may reduce fusion-output rates;
its
100 Hz example concerns Game Rotation Vector. Within one run the order is safe:
`b-rv400` runs before `b-girv`. It is the next run that inherits the state.

## Records

| File | Profile | Started (UTC) | Image |
|---|---|---|---|
| `pico2-v0.1-default.json` | default | 2026-11-03 22:24 | pico-bno085 0.1.1, `272549a`; hash not recorded |
| `pico2-v0.1-large.json` | large | 2026-11-03 23:27 | pico-bno085 0.1.1, `272549a`; bench image SHA-256 `7887a3f690653be8fc6b68158449f7a713f7c4c594e2843190c42ca53add57e1` |

Both: Pico 2 W with a BNO085, wired per the usage guide; session bus SPI;
firmware `CMAKE_BUILD_TYPE` Release, `PICO_PLATFORM` rp2350-arm-s, Pico SDK
2.3.0, arm-none-eabi-gcc 14.2.1; started from a fresh flash of the bench
image; all twelve cases, `b-soak` for 3600 s. Every case passed on both
profiles. In `b-i2c100` sensor 0x01 delivered 126.9 Hz at the 100 Hz
requested on both profiles; the gate passes it, and the cause is not
established.
