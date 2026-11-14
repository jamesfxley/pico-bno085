# Using pico-bno085

This guide covers wiring and startup on a Pico 2 W. The
[specification](technical-specification.md) defines guarantees, and the
[architecture](architecture.md) explains the implementation.

## Wiring

One wiring supports both I2C and SPI. The protocol-select pins (PS0/PS1) are
driven by the Pico before NRST is released, so the bus is chosen in firmware
without changing jumpers. GPIO numbers are the defaults in
[pins.hpp](../src/hal/rp2350/include/pico_bno085/hal/pins.hpp); the Pico column
is the 40-pin header position of that GPIO.

## Pin map

Chip pin numbers and names are the BNO08X datasheet's (1000-3927 §1.2.1); the
same pinout covers BNO085 and BNO086. The datasheet calls the part the *sensor
hub* and the MCU the *host*, so "hub" below is the BNO085 and "host" the Pico.

| Chip pin | Datasheet name | Pico GPIO | Pico pin | Purpose |
|----------|----------------|-----------|----------|---------|
| 1 | RESV_NC | — | — | no connect |
| 2 | GND | GND | 3/8/13/18/23/28/38 | ground |
| 3 | VDD | 3V3(OUT) | 36 | sensor supply, 2.4–3.6 V |
| 4 | BOOTN | GP10 | 14 | driven high: run the application, not the bootloader |
| 5 | PS1 | GP9 | 12 | protocol select, sampled at reset |
| 6 | PS0/WAKE | GP8 | 11 | protocol select; active-low WAKE in SPI mode |
| 7, 8 | RESV_NC | — | — | no connect |
| 9 | CAP | — | — | 100 nF to GND |
| 10 | CLKSEL0 | — | — | clock source select, internal pulldown |
| 11 | NRST | GP7 | 10 | hub reset, active low |
| 12, 13 | RESV_NC | — | — | no connect |
| 14 | H_INTN | GP6 | 9 | host interrupt, active low; input, pull-up, falling IRQ |
| 15 | ENV_SCL | — | — | secondary I2C, pull up to VDDIO even if unused |
| 16 | ENV_SDA | — | — | secondary I2C, pull up to VDDIO even if unused |
| 17 | SA0/H_MOSI | GP19 | 25 | SPI data-in; I2C address bit (driven low → 0x4A) |
| 18 | H_CSN | GP17 | 22 | SPI chip select, driven as a plain GPIO |
| 19 | H_SCL/SCK | GP3 and GP18 | 5 and 24 | I2C clock / SPI clock |
| 20 | H_SDA/H_MISO | GP2 and GP16 | 4 and 21 | I2C data / SPI data-out |
| 21–24 | RESV_NC | — | — | no connect |
| 25 | GND | GND | 3/8/13/18/23/28/38 | ground |
| 26 | XOUT32/CLKSEL1 | — | — | 32.768 kHz crystal / clock select |
| 27 | XIN32 | — | — | 32.768 kHz crystal or external clock |
| 28 | VDDIO | 3V3(OUT) | 36 | core and I/O supply, 1.65–3.6 V |

Add 2–4 kΩ pull-ups to VDDIO on chip pins 19 and 20 (the datasheet's R1/R2) for
the I2C phase, unless the module already carries them. They stay in place for
SPI: the push-pull SPI drivers overcome them, at the cost of ~1 mA while a line
is held low.

### Pins the BNO085 multiplexes

Three chip pins carry a different function per bus, which constrains the wiring:

- Pins 19 and 20 need two Pico GPIOs each, because no RP2350 pin can be both
  an I2C clock and a SPI clock. The RP2350 datasheet (Table 677) fixes the
  residue rules: I2C SCL is only available on odd-numbered GPIOs and SPI SCK
  only on GPIOs ≡ 2 (mod 4). The I2C pair is further constrained to
  one peripheral; `i2c0` reaches SDA on GPIOs ≡ 0 (mod 4) and SCL on ≡ 1, `i2c1`
  SDA on ≡ 2 and SCL on ≡ 3 — so GP2/GP3 select `i2c1`, which
  [pins.hpp](../src/hal/rp2350/include/pico_bno085/hal/pins.hpp) binds by
  `static_assert`. Both ends of each net are wired
  permanently; only the selected bus owns its pads. An unconfigured pad presents
  at most a weak pull-down, which the external pull-up overcomes.
- Pin 17 is the I2C address bit before reset and SPI data-in after, so it
  gets one GPIO: `ControlPins` drives GP19 low as an I2C-only strap, and while
  SPI is selected leaves it to the SPI transport. Only one driver is active on
  that net (HR-3).

The datasheet's I2C wiring guidance places no requirement on H_CSN, so its
jumper stays connected across both phases and the pad is simply left
unconfigured while I2C is selected.

## Wiring a GY-BNO08X module

A module supplies the CAP, clock, ENV bus, and pull-up components itself and
brings out one header pin per chip pin, so nothing is doubled on the module
side. The GY-BNO08X carries a 10-pin header:

| Module pin | Chip pin | Pico GPIO | Pico pin |
|------------|----------|-----------|----------|
| VCC | 3, 28 (VDD, VDDIO) | 3V3(OUT) | 36 |
| GND | 2, 25 | GND | 38 |
| SCL | 19 (H_SCL/SCK) | GP3 and GP18 | 5 and 24 |
| SDA | 20 (H_SDA/H_MISO) | GP2 and GP16 | 4 and 21 |
| ADD | 17 (SA0/H_MOSI) | GP19 | 25 |
| CS | 18 (H_CSN) | GP17 | 22 |
| INT | 14 (H_INTN) | GP6 | 9 |
| RST | 11 (NRST) | GP7 | 10 |
| PS1 | 5 | GP9 | 12 |
| PS0 | 6 (PS0/WAKE) | GP8 | 11 |

`ADD` is the address-select label for the pin the datasheet calls SA0/H_MOSI,
so it is also SPI data-in; likewise `SCL` is the SPI clock and `SDA` the SPI
data-out. BOOTN is not on the header, so GP10 stays unwired — the module holds
it high, which is the application-boot level the driver would otherwise drive.
This module wiring is UNCONFIRMED: no module schematic is on record.

Power VCC from 3V3(OUT), never VSYS: the module's I2C pull-ups tie to VCC, so
VCC sets the bus idle level and the Pico's pads see it directly.

Before powering it, with the board disconnected, measure `ADD`, `PS0`, and
`PS1` against GND and VCC. The firmware drives all three (`ADD` low in I2C
mode; PS0/PS1 high for SPI, low for I2C), so a 0 Ω strap on any of them is a
short the moment it is driven the other way, and the strap has to come off
before that bus is used. A few kΩ is a pull resistor and is safe to drive.

## Power

VDD (chip pin 3) must reach its level before or with VDDIO (chip pin 28); tying
both to the Pico's 3V3(OUT) satisfies the ordering (1000-3927 §6.3). The
secondary environmental I2C bus (ENV_SCL/ENV_SDA) must be pulled up even with no
sensor fitted, because the hub firmware polls it at reset.

## Protocol select

| PS1 | PS0 | Application mode (BOOTN high) |
|-----|-----|------------------------------|
| 0 | 0 | I2C |
| 1 | 1 | SPI |

The 0/1 and 1/0 combinations select the UART modes, which are out of scope for
this driver (FW-5). In SPI mode both pins must be high from before reset
until after the first H_INTN assertion. PS0 then becomes active-low WAKE
(1000-3927 §1.2.4).

## Startup

After NRST is released the hub takes at least 90 ms (datasheet t1) to assert
H_INTN for the first time, then emits the SHTP advertisement, an
executable-channel reset-complete, and an unsolicited SH-2 initialize response.
The driver's `begin()` sequences this without blocking; see the pico-sh2 session
specification for the ordering requirements.

## Flash and run

```sh
python3 dev.py build --tree firmware   # builds both example images
```

Follow [the pinned family device guide](../infra/guides/device.md) to flash
`build-firmware/examples/minimal/pico_bno085_minimal.uf2` or the corresponding
bench image and open its console. Device actions require specific authorisation.
Close the console before benchmarking: the clients share an exclusive serial
port.

The minimal image resets the hub, enables the rotation vector at 50 Hz, and
streams `rv i=.. j=.. k=.. r=..` once the session reaches READY.

If nothing streams, check the startup fault. `begin()` is non-blocking and
`init()` configures the peripheral without addressing the hub. The example's
loop reports a failed handshake:

```
bno085 startup faulted in phase 3, error 2
```

The phase is a `pico_sh2::StartupPhase` and the error a `pico_bno085::Error`
(TR-1). `WAIT_ADVERTISEMENT` with `TIMEOUT` means no advertisement arrived.
Possible causes include a wrong address, an unpowered part or straps selecting
the other bus. `WAIT_RESET_COMPLETE` or `WAIT_INITIALIZE` means the
handshake failed after the advertisement. Framing or bus integrity can cause
these failures.

Then check H_INTN, the protocol-select straps and the SA0 address.
Without a HINTN assertion, the driver issues no bus reads (HR-1).

## Validation

One image supports both buses. The harness selects the bus at run time:

```sh
python3 dev.py build --tree firmware
# Flash the bench image using the pinned family device guide, then:
python3 examples/bench/bench.py --port "$RUN/bno-tty" --bus spi --tests b-boot,b-rate
```

The [benchmark guide](../examples/bench/README.md) describes `--bus`,
case-specific bus selection and the command protocol. Bus changes re-strap
with NRST asserted (HR-3). The hub restarts and loses session state.

`trace 1` shows raw transfers. `boot` retries startup after a wiring change
without reflashing. `info` reports the firmware constants used by the harness.

Drive the bench through the proxy by setting `BOARD_SERIAL` to the authorised
serial and `RUN` to a disposable folder in both terminals. In one terminal:

```sh
socat PTY,link="$RUN/bno-tty",raw,echo=0 EXEC:"python3 -m infra.pico_tooling device console --serial $BOARD_SERIAL"
```

Once the PTY exists, run the bench command above in the other terminal. Stop
the bridge with Ctrl+C afterwards. `--port` opens that path directly; it does
not search for ports. The
[pinned device guide](../infra/guides/device.md) owns flash and console setup.

See [examples/bench/baselines/README.md](../examples/bench/baselines/README.md)
for the promoted-result format and the conditions to record with each run.

The RP2350 pin-mux rules (datasheet Table 677) are checked in code. For I2C the
peripheral is (GPIO / 2) % 2, SDA on even GPIOs and SCL on odd ones. For SPI the
peripheral is (GPIO / 8) % 2, and GPIO % 4 selects RX (MISO) 0, SCK 2 or TX
(MOSI) 3; CSN is driven as a plain GPIO and may be any GPIO of bank 0. GPIOs 30
to 47 exist on the 80-pin package only. `pins.hpp` binds its defaults to these
rules by `static_assert`, and each transport's `init()` returns
`INVALID_ARGUMENT`, before any pin is touched, for a pin the selected
peripheral cannot reach or a GPIO outside bank 0.
