#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Drive the device checklist over the serial protocol in README.md.

The heap command reports growth after construction (AR-1).
The heap case checks zero growth (AR-1).
RV400 checks rotation-vector delivery (PR-1).
Timestamp cases check ordering (PR-3) and record RV-to-gyro phase (PR-3).
I2C100 checks concurrent sensors (PR-2). GIRV checks integrated RV (PR-4).
Latency checks HINTN service (HR-5). Parity compares buses (HR-4).
The soak checks counters over a long run (OR-1).

Results print as a table. --promote writes them beside the script in baselines/.
Pass --bus and a serial path as described in README.md. No port is discovered.
Requires pyserial; the remaining imports use the Python standard library.
"""

import argparse
import bisect
import datetime
import itertools
import json
import sys
import time
from collections import defaultdict, deque
from pathlib import Path

try:
    import serial
except ImportError:
    serial = None

ALL_TESTS = [
    "b-boot", "b-rate", "b-rv400", "b-ts", "b-rvphase", "b-i2c100", "b-girv",
    "b-lat", "b-heap", "b-parity", "b-reset", "b-soak",
]

# Tests that take a long time; excluded from a default run and asked for by name.
SLOW_TESTS = {"b-soak"}

# Granted-rate factors (1000-3927 §6.9). All rate gates use this band.
GRANTED_RATE_FLOOR = 0.9
GRANTED_RATE_CEILING = 2.1


def within_granted_band(delivered_hz, requested_hz):
    """Whether a delivered rate sits inside the datasheet's granted-rate band."""
    return (GRANTED_RATE_FLOOR * requested_hz <= delivered_hz
            <= GRANTED_RATE_CEILING * requested_hz)


# SH-2 report IDs of the rotation-vector-class sensors used by the default runs.
ROTATION_VECTOR = 0x05
READY_TIMEOUT_S = 3.0
# Quiet observation interval in seconds for b-boot (HR-1).
QUIET_INTERVAL_S = 2.0
# Reset to READY plus the re-enable of every desired sensor.
RECOVERY_TIMEOUT_S = 5.0

# Default tick in microseconds and ordering bound in ticks
# (1000-3625 §7.2.1) (PR-3).
DEFAULT_TICK_US = 100
RECONSTRUCTION_TICKS = 4
# Minimum sample interval in reconstruction bounds.
PHASE_INTERVAL_BOUNDS = 10
# Relative delivered-rate tolerance for treating two streams as one rate.
PHASE_RATE_TOLERANCE = 0.01

# The gyro-integrated rotation vector, which has its own SHTP channel.
GYRO_INTEGRATED_RV = 0x2A

# Concurrent I2C test sensors (PR-2).
ACCELEROMETER = 0x01
GYROSCOPE = 0x02
MAGNETOMETER = 0x03

# Sensor IDs disabled for per-case isolation (AR-3).
HARNESS_SENSORS = (0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x08, 0x09, 0x0F,
                   GYRO_INTEGRATED_RV)

def decode_wire_input(edge_us, cargo):
    """Decode RV/gyro timing from a complete SH-2 input cargo."""
    # Signed base/rebase and 100 us ticks: 1000-3625 §§7.2.1-7.2.2.
    # Delay addition follows 1000-3927 §1.3.5.3's example despite its prose
    # disagreeing (PR-3). Lengths: 1000-3625 §§6.5.13, 6.5.18. Prefix: §6.5.1.
    lengths = {GYROSCOPE: 10, ROTATION_VECTOR: 14, 0xFB: 5, 0xFA: 5}
    events = []
    base = None
    rebases = 0
    offset = 0
    while offset < len(cargo):
        report_id = cargo[offset]
        length = lengths.get(report_id)
        if length is None or offset + length > len(cargo):
            raise ValueError("unknown or truncated input report")
        report = cargo[offset:offset + length]
        if report_id == 0xFB:
            base = int.from_bytes(report[1:5], "little", signed=True)
            rebases = 0
            if base == 0x7FFFFFFF:
                raise ValueError("overflowed base has no reconstructable time")
        elif report_id == 0xFA:
            rebases += int.from_bytes(report[1:5], "little", signed=True)
        else:
            if base is None:
                raise ValueError("input report without a base timestamp")
            delay = ((report[2] >> 2) << 8) | report[3]
            event_us = edge_us + 100 * (-base + rebases + delay)
            events.append({"sensor": report_id, "seq": report[1],
                           "event_us": event_us, "edge_us": edge_us,
                           "base_delta": base, "rebases": rebases,
                           "delay_ticks": delay, "cargo_offset": offset})
        offset += length
    return events


def decode_wire_transfer(edge_us, frame):
    """Return timing events and grants. Reject incomplete transfers."""
    # The bench traces SHTP header + cargo (1000-3927 §1.3.1).
    if len(frame) < 4:
        raise ValueError("short transfer header")
    length = int.from_bytes(frame[:2], "little")
    if length == 0:
        return [], []
    if length & 0x8000 or length != len(frame):
        raise ValueError("fragmented or length-mismatched transfer")
    channel, cargo = frame[2], frame[4:]
    if channel in (3, 4):
        return decode_wire_input(edge_us, cargo), []
    grants = []
    if channel == 2 and cargo and cargo[0] == 0xFC:
        # 1000-3625 §6.5.5: full feature report, unsolicited on rate changes.
        if len(cargo) != 17:
            raise ValueError("truncated or combined feature response")
        grants.append({"sensor": cargo[1], "flags": cargo[2],
                       "change_sensitivity": int.from_bytes(cargo[3:5], "little"),
                       "report_interval_us": int.from_bytes(cargo[5:9], "little"),
                       "batch_interval_us": int.from_bytes(cargo[9:13], "little"),
                       "sensor_specific": int.from_bytes(cargo[13:17], "little")})
    return [], grants



def read_wire_line(line):
    header, payload = line.split(":", 1)
    fields = dict(item.split("=", 1) for item in header.split()[2:])
    frame = bytes.fromhex(payload)
    if len(frame) != int(fields["n"]):
        raise ValueError("wire trace byte count mismatch")
    return int(fields["edge_us"]), frame, payload


class WireCapture:
    """Diagnostic wire evidence, independent of the phase verdict."""

    def __init__(self):
        self.frames = []
        self.events = []
        self.grants = []
        self.errors = []
        self.in_window = False

    def feed(self, line):
        if line.startswith("# run "):
            self.in_window = True
        elif line.startswith("# done"):
            self.in_window = False
        elif line.startswith("# wire "):
            self.read_transfer(line)

    def read_transfer(self, line):
        try:
            edge, frame, payload = read_wire_line(line)
            events, grants = decode_wire_transfer(edge, frame)
        except (ValueError, KeyError) as error:
            self.errors.append({"line": line, "error": str(error)})
            return
        index = len(self.frames)
        self.frames.append({"edge_us": edge, "hex": payload.strip()})
        if self.in_window:
            self.events.extend({**event, "frame": index} for event in events)
        self.grants.extend({**grant, "frame": index} for grant in grants)

    def compare(self, driver_events):
        pending = defaultdict(deque)
        for event in self.events:
            pending[event["sensor"], event["seq"]].append(event)
        pairs = []
        missing = []
        for event in driver_events:
            if event["sensor"] not in (GYROSCOPE, ROTATION_VECTOR):
                continue
            queue = pending[event["sensor"], event["seq"]]
            if not queue:
                missing.append(event)
                continue
            wire = queue.popleft()
            delta = event["event_us"] - wire["event_us"]
            pairs.append({"sensor": event["sensor"], "seq": event["seq"],
                          "driver_us": event["event_us"], "wire": wire,
                          "driver_minus_wire_us": delta, "agree": delta == 0})
        offsets = nearest_offsets(only(self.events, ROTATION_VECTOR),
                                  only(self.events, GYROSCOPE))
        extra = [event for queue in pending.values() for event in queue]
        summary = {"matched": len(pairs),
                   "disagreements": sum(not pair["agree"] for pair in pairs),
                   "unmatched_driver": len(missing), "unmatched_wire": len(extra),
                   "decode_errors": len(self.errors),
                   "wire_median_offset_us": (sorted(offsets)[len(offsets) // 2]
                                             if offsets else None),
                   "wire_worst_offset_us": max(map(abs, offsets), default=None),
                   "granted_intervals": self.grants}
        return {"summary": summary, "pairs": pairs, "unmatched_driver": missing,
                "unmatched_wire": extra, "wire_phase_offsets_us": offsets,
                "errors": self.errors, "frames": self.frames}


class Device:
    """Line-oriented view of the bench firmware over a serial port."""

    def __init__(self, port, baud, timeout):
        if serial is None:
            sys.exit("bench.py requires pyserial: pip install pyserial")
        # USB CDC ignores the baud rate, but pyserial still wants one.
        self.ser = serial.Serial(port, baud, timeout=timeout)
        self.bus = None
        self.wire = None

    def send(self, line):
        self.ser.write((line + "\n").encode("ascii"))
        self.ser.flush()

    def readline(self):
        line = self.ser.readline().decode("ascii", "replace").strip()
        if self.wire is not None:
            self.wire.feed(line)
        return line

    def wait_for(self, prefix, deadline_s):
        """Read lines until one starts with prefix or the deadline passes."""
        end = time.monotonic() + deadline_s
        while time.monotonic() < end:
            line = self.readline()
            if line.startswith(prefix):
                return line
        return None

    def collect_run(self, duration_ms, reconfigure=None):
        """Send run and return (events, stats) through the done reply.

        The grace period has to exceed the read timeout because readline() can
        consume one timeout before the deadline is checked again.
        """
        self.send(f"run {duration_ms}")
        events = []
        stats = {}
        grace_s = max(2.0, self.ser.timeout * 3 if self.ser.timeout else 2.0)
        started = time.monotonic()
        deadline = started + duration_ms / 1000.0 + grace_s
        sent = confirmed = False
        while time.monotonic() < deadline:
            if (reconfigure is not None and not sent
                    and time.monotonic() >= started + duration_ms / 2000.0):
                sensor, interval = reconfigure
                self.send(f"cfg {sensor:02x} {interval}")
                sent = True
            line = self.readline()
            if not line:
                continue
            if line.startswith("E,"):
                events.append(parse_event(line))
            elif sent and line.startswith("# ok"):
                confirmed = True
            elif line.startswith("# stats"):
                stats = parse_stats(line)
            elif line.startswith("# done"):
                break
        if reconfigure is not None:
            stats["midstream_reconfigured"] = confirmed
        return events, stats


def parse_event(line):
    # E,<sensor>,<seq>,<event_us>,<accuracy>
    _, sensor, seq, event_us, accuracy = line.split(",")
    return {
        "sensor": int(sensor),
        "seq": int(seq),
        "event_us": int(event_us),
        "accuracy": int(accuracy),
    }


def parse_stats(line):
    stats = {}
    for token in line[len("# stats"):].split():
        key, _, value = token.partition("=")
        if value.isdigit():
            stats[key] = int(value)
    return stats


def only(events, *sensors):
    """Select events by sensor so each stream is measured separately."""
    wanted = set(sensors)
    return [event for event in events if event["sensor"] in wanted]


def sequence_gaps(events):
    """Count per-sensor 8-bit sequence discontinuities (dropped reports)."""
    last = {}
    gaps = 0
    for event in events:
        sensor = event["sensor"]
        seq = event["seq"]
        if sensor in last:
            expected = (last[sensor] + 1) & 0xFF
            if seq != expected:
                gaps += (seq - expected) & 0xFF
        last[sensor] = seq
    return gaps


def delivered_rate_hz(events):
    if len(events) < 2:
        return 0.0
    span_us = events[-1]["event_us"] - events[0]["event_us"]
    if span_us <= 0:
        return 0.0
    return (len(events) - 1) * 1_000_000.0 / span_us


def timestamp_jitter(events, interval_us):
    """Per-sensor inter-arrival deltas; return (monotonic, p99_ratio)."""
    monotonic, worst_ratio, _, _ = timestamp_detail(events, interval_us)
    return monotonic, worst_ratio


def timestamp_detail(events, interval_us):
    """Return (monotonic, p99_ratio, inversions, worst_delta_us)."""
    by_sensor = {}
    for event in events:
        by_sensor.setdefault(event["sensor"], []).append(event["event_us"])
    monotonic = True
    worst_ratio = 0.0
    inversions = 0
    worst_delta = 0
    for times in by_sensor.values():
        deltas = [b - a for a, b in itertools.pairwise(times)]
        backward = [d for d in deltas if d <= 0]
        if backward:
            monotonic = False
            inversions += len(backward)
            worst_delta = min(worst_delta, *backward)
        if deltas and interval_us > 0:
            ordered = sorted(deltas)
            p99 = ordered[min(len(ordered) - 1, int(0.99 * len(ordered)))]
            worst_ratio = max(worst_ratio, abs(p99 - interval_us) / interval_us)
    return monotonic, worst_ratio, inversions, worst_delta


def read_info(dev):
    """Read constants reported by the firmware."""
    dev.send("info")
    line = dev.wait_for("# info", 2.0) or ""
    info = {}
    for token in line[len("# info"):].split():
        key, _, value = token.partition("=")
        info[key] = int(value) if value.isdigit() else value
    return info


def reconstruction_bound_us(dev):
    return RECONSTRUCTION_TICKS * read_info(dev).get("tick_us", DEFAULT_TICK_US)


# A run shorter than 60 s does not establish sustained delivery (PR-1).
PR1_SECONDS = 60.0

# The cases that select buses of their own.
OWN_BUS = {"b-rv400": "spi", "b-girv": "spi", "b-i2c100": "i2c", "b-parity": "both"}


def select_bus(dev, bus):
    """Select a bus by re-strapping with the hub held in reset."""
    dev.bus = None
    dev.send(f"bus {bus}")
    reply = dev.wait_for(f"# bus {bus}", 3.0)
    if reply is None:
        return False
    dev.bus = reply.split()[2]
    return wait_ready(dev)


def disable_payload(dev):
    """Turn payload lines off. They replace the E lines the cases count (LP-5)."""
    dev.send("payload 0")
    return dev.wait_for("# payload 0", 2.0) is not None


def start_session(dev, bus):
    """Turn payload off, which an earlier session can have left on, then select bus."""
    if not disable_payload(dev):
        print("warning: the firmware did not acknowledge 'payload 0'; is it "
              "an older image?")
    return select_bus(dev, bus)


def snapshot(dev):
    """Read cumulative counters. Per-run counts are differences between readings."""
    dev.send("stat")
    return parse_stats(dev.wait_for("# stats", 2.0) or "# stats")


def during(before, after, key):
    return after.get(key, 0) - before.get(key, 0)


def ack(dev):
    """Wait for a command acknowledgement without consuming other status replies."""
    line = dev.wait_for("# ", 2.0)
    while line is not None and not line.startswith(("# ok", "# err")):
        line = dev.wait_for("# ", 2.0)
    return line is not None and line.startswith("# ok")


def isolate(dev, *sensors):
    """Disable every sensor outside the requested set, which survives reset (AR-3)."""
    for sensor in HARNESS_SENSORS:
        if sensor not in sensors:
            dev.send(f"off {sensor:02x}")
            ack(dev)


def enable(dev, sensor, interval_us):
    dev.send(f"cfg {sensor:02x} {interval_us}")
    return ack(dev)


def case_boot(dev, _cfg):
    # Disable other sensors before timing boot.
    isolate(dev)
    # Executable-channel reset leaves calibration survival UNCONFIRMED (FW-2).
    dev.send("reset")
    start = time.monotonic()
    ready = dev.wait_for("# ready", READY_TIMEOUT_S)
    elapsed_ms = (time.monotonic() - start) * 1000.0

    # absent_reads stays at zero while the hub stays on the bus. It cannot
    # detect blind reads of a present hub (HR-1); bno085_test
    # ATransportReportingNoDataIsNeverRead holds that on the host (HR-1).
    before = snapshot(dev)
    time.sleep(QUIET_INTERVAL_S)
    after = snapshot(dev)
    quiet_reads = during(before, after, "absent_reads")

    return {
        "test": "b-boot",
        "passed": ready is not None and quiet_reads == 0,
        "boot_ms": round(elapsed_ms, 1),
        "quiet_absent_reads": quiet_reads,
        "quiet_interval_s": QUIET_INTERVAL_S,
    }


def case_rate(dev, cfg):
    interval_us = round(1_000_000 / cfg.rate_hz)
    isolate(dev, cfg.sensor)
    enable(dev, cfg.sensor, interval_us)
    before = snapshot(dev)
    events, stats = dev.collect_run(cfg.duration * 1000)
    events = only(events, cfg.sensor)
    rate = delivered_rate_hz(events)
    gaps = sequence_gaps(events)
    drops = during(before, stats, "reassembly_drops")
    rate_ok = within_granted_band(rate, cfg.rate_hz)
    clean = gaps == 0 and drops == 0
    return {
        "test": "b-rate",
        "passed": bool(events) and rate_ok and clean,
        "requested_hz": cfg.rate_hz,
        "delivered_hz": round(rate, 1),
        "events": len(events),
        "seq_gaps": gaps,
        "reasm_drops": drops,
    }


def case_ts(dev, cfg):
    interval_us = round(1_000_000 / cfg.rate_hz)
    bound_us = reconstruction_bound_us(dev)
    isolate(dev, cfg.sensor)
    enable(dev, cfg.sensor, interval_us)
    events, stats = dev.collect_run(cfg.duration * 1000)
    events = only(events, cfg.sensor)
    monotonic, jitter_ratio, inversions, worst_delta = timestamp_detail(
        events, interval_us)
    # Judge backward steps at the reconstruction bound, since base delta and
    # report delay are quantised to 100 us ticks (PR-3).
    ordered = worst_delta > -bound_us
    return {
        "test": "b-ts",
        "passed": bool(events) and ordered and jitter_ratio <= 1.0,
        "monotonic": monotonic,
        "ordered_to_tick": ordered,
        "inversions": inversions,
        "worst_delta_us": worst_delta,
        "p99_jitter_intervals": round(jitter_ratio, 3),
        "events": len(events),
        # Level-fallback reads can use an earlier edge and explain jitter.
        "level_fallbacks": stats.get("level_fallbacks", 0),
    }


def nearest_offsets(events, reference):
    """Each event's time minus the nearest reference event's time."""
    times = sorted(event["event_us"] for event in reference)
    offsets = []
    for event in events:
        t = event["event_us"]
        i = bisect.bisect_left(times, t)
        near = [times[j] for j in (i - 1, i) if 0 <= j < len(times)]
        if near:
            offsets.append(t - min(near, key=lambda g: abs(g - t)))
    return offsets


def case_rvphase(dev, cfg):
    """Record RV-to-gyro phase without gating it (PR-3).

    The reports' interrupt reference and shared sample phase are UNCONFIRMED
    (1000-3927 §6.8, 1000-3625) (FW-2). Validity requires one delivered rate within
    the granted band and an interval that separates the sample grid from the bound.
    Nearest-time pairing measures phase modulo the interval (PR-3).
    """
    interval_us = round(1_000_000 / cfg.rate_hz)
    bound_us = reconstruction_bound_us(dev)
    if cfg.wire:
        dev.wire = WireCapture()
        dev.send("trace 2")
        if dev.wait_for("# trace 2", 2.0) is None:
            dev.wire.errors.append("trace 2 acknowledgement missing")
    isolate(dev, ROTATION_VECTOR, GYROSCOPE)
    enable(dev, GYROSCOPE, interval_us)
    enable(dev, ROTATION_VECTOR, interval_us)
    before = snapshot(dev)
    events, stats = dev.collect_run(cfg.duration * 1000)
    rv = only(events, ROTATION_VECTOR)
    gyro = only(events, GYROSCOPE)
    rv_hz = delivered_rate_hz(rv)
    gyro_hz = delivered_rate_hz(gyro)
    one_rate = (within_granted_band(rv_hz, cfg.rate_hz)
                and within_granted_band(gyro_hz, cfg.rate_hz)
                and abs(rv_hz - gyro_hz) <= PHASE_RATE_TOLERANCE * gyro_hz)
    separable = (gyro_hz > 0
                 and 1_000_000 / gyro_hz >= PHASE_INTERVAL_BOUNDS * bound_us)
    offsets = nearest_offsets(rv, gyro)
    worst = max((abs(offset) for offset in offsets), default=None)
    result = {
        "test": "b-rvphase",
        "passed": bool(offsets) and one_rate and separable,
        "rv_hz": round(rv_hz, 1),
        "gyro_hz": round(gyro_hz, 1),
        "rv_events": len(rv),
        "median_offset_us": sorted(offsets)[len(offsets) // 2] if offsets else None,
        "worst_offset_us": worst,
        "bound_us": bound_us,
        # Level-fallback reads can use an earlier edge and explain a phase offset.
        "level_fallbacks": during(before, stats, "level_fallbacks"),
    }

    if cfg.wire:
        dev.send("trace 0")
        if dev.wait_for("# trace 0", 2.0) is None:
            dev.wire.errors.append("trace 0 acknowledgement missing")
        diagnostic = dev.wire.compare(events)
        path = Path(cfg.output_dir)
        path.mkdir(parents=True, exist_ok=True)
        stamp = datetime.datetime.now(datetime.UTC).strftime("%Y%m%d-%H%M%S-%f")
        sidecar = path / f"b-rvphase-wire-{stamp}.json"
        sidecar.write_text(json.dumps(diagnostic, indent=2) + "\n", encoding="ascii")
        result["wire"] = {**diagnostic["summary"], "sidecar": str(sidecar)}
        dev.wire = None
    return result


def case_i2c100(dev, cfg):
    """Run accel, gyro and mag at 100 Hz over I2C without drops (PR-2)."""
    interval_us = round(1_000_000 / cfg.i2c100_hz)
    sensors = (ACCELEROMETER, GYROSCOPE, MAGNETOMETER)
    # The concurrent-sensor case uses I2C (PR-2).
    if not select_bus(dev, "i2c"):
        return {"test": "b-i2c100", "passed": False, "bus": dev.bus}
    bound_us = reconstruction_bound_us(dev)
    isolate(dev, *sensors)
    for sensor in sensors:
        enable(dev, sensor, interval_us)
    before = snapshot(dev)
    events, stats = dev.collect_run(cfg.i2c100_duration * 1000)
    drops = during(before, stats, "reassembly_drops")
    result = {
        "test": "b-i2c100",
        "passed": True,
        "bus": dev.bus,
        "requested_hz": cfg.i2c100_hz,
        "seconds": cfg.i2c100_duration,
        "reasm_drops": drops,
        "level_fallbacks": during(before, stats, "level_fallbacks"),
    }
    if drops != 0:
        result["passed"] = False
    for sensor in sensors:
        rows = only(events, sensor)
        rate = delivered_rate_hz(rows)
        gaps = sequence_gaps(rows)
        _, _, _, worst_delta = timestamp_detail(rows, interval_us)
        ordered = worst_delta > -bound_us
        name = f"{sensor:02x}"
        result[f"hz_{name}"] = round(rate, 1)
        result[f"gaps_{name}"] = gaps
        result[f"ordered_{name}"] = ordered
        # Ordering uses tick resolution (PR-3). Rates use the granted-rate band.
        if (not rows or gaps != 0 or not ordered
                or not within_granted_band(rate, cfg.i2c100_hz)):
            result["passed"] = False
    return result


def case_reset(dev, cfg):
    interval_us = round(1_000_000 / cfg.rate_hz)
    enable(dev, cfg.sensor, interval_us)
    before = snapshot(dev)
    want_reenabled = before.get("enabled", 0)
    start = time.monotonic()
    # Executable-channel reset tests recovery without clearing calibration.
    dev.send("reset")
    ready = dev.wait_for("# ready", READY_TIMEOUT_S)
    # READY precedes completion of sensor reapplication (AR-3). Poll until
    # the recovery counters settle.
    after = {}
    while time.monotonic() - start < RECOVERY_TIMEOUT_S:
        dev.send("stat")
        after = parse_stats(dev.wait_for("# stats", 2.0) or "# stats")
        if (after.get("recoveries", 0) > before.get("recoveries", 0)
                and after.get("reenabled", 0)
                >= before.get("reenabled", 0) + want_reenabled):
            break
    recovery_ms = (time.monotonic() - start) * 1000.0
    recovered = after.get("recoveries", 0) > before.get("recoveries", 0)
    reenabled = (after.get("reenabled", 0)
                 >= before.get("reenabled", 0) + want_reenabled)
    return {
        "test": "b-reset",
        "passed": ready is not None and recovered and reenabled,
        "recovery_ms": round(recovery_ms, 1),
        "sensors_to_restore": want_reenabled,
        "recoveries": after.get("recoveries", 0),
        "reenabled": after.get("reenabled", 0),
    }


def case_rv400(dev, cfg):
    """Run rotation vector at 400 Hz over SPI without drops (PR-1).

    SPI is selected for the requirement (PR-1).
    """
    interval_us = round(1_000_000 / cfg.rv400_hz)
    if not select_bus(dev, "spi"):
        return {"test": "b-rv400", "passed": False, "bus": dev.bus}
    bound_us = reconstruction_bound_us(dev)
    isolate(dev, ROTATION_VECTOR)
    enable(dev, ROTATION_VECTOR, interval_us)
    before = snapshot(dev)
    # Retain events while a write reconfigures a second sensor (HR-4).
    events, stats = dev.collect_run(
        cfg.rv400_duration * 1000, reconfigure=(ACCELEROMETER, 20_000))
    reconfigured = stats.get("midstream_reconfigured", False)
    events = only(events, ROTATION_VECTOR)
    rate = delivered_rate_hz(events)
    gaps = sequence_gaps(events)
    drops = during(before, stats, "reassembly_drops")
    _, _, _, worst_delta = timestamp_detail(events, interval_us)
    ordered = worst_delta > -bound_us
    return {
        "test": "b-rv400",
        "bus": dev.bus,
        "passed": reconfigured and bool(events) and gaps == 0 and drops == 0 and ordered
                  and within_granted_band(rate, cfg.rv400_hz)
                  and cfg.rv400_duration >= PR1_SECONDS,
        "midstream_reconfigured": reconfigured,
        "requested_hz": cfg.rv400_hz,
        "delivered_hz": round(rate, 1),
        "seconds": cfg.rv400_duration,
        "events": len(events),
        "seq_gaps": gaps,
        "reasm_drops": drops,
        "worst_delta_us": worst_delta,
    }


def case_girv(dev, cfg):
    """Run gyro-integrated RV over SPI on its own channel (PR-4).

    SPI is selected for the requirement (PR-4).
    """
    interval_us = round(1_000_000 / cfg.girv_hz)
    if not select_bus(dev, "spi"):
        return {"test": "b-girv", "passed": False, "bus": dev.bus}
    isolate(dev, GYRO_INTEGRATED_RV)
    enable(dev, GYRO_INTEGRATED_RV, interval_us)
    before = snapshot(dev)
    events, stats = dev.collect_run(cfg.duration * 1000)
    events = only(events, GYRO_INTEGRATED_RV)
    rate = delivered_rate_hz(events)
    drops = during(before, stats, "reassembly_drops")
    return {
        "test": "b-girv",
        "bus": dev.bus,
        "passed": (bool(events) and drops == 0
                   and within_granted_band(rate, cfg.girv_hz)),
        "requested_hz": cfg.girv_hz,
        "delivered_hz": round(rate, 1),
        "events": len(events),
        "reasm_drops": drops,
    }


def case_lat(dev, cfg):
    """Measure the worst HINTN-to-read gap under load (HR-5)."""
    interval_us = round(1_000_000 / cfg.rate_hz)
    isolate(dev, cfg.sensor)
    enable(dev, cfg.sensor, interval_us)
    before = snapshot(dev)
    _, stats = dev.collect_run(cfg.duration * 1000)
    # The firmware clears the latency peak for each run, excluding startup waits.
    worst = stats.get("service_latency_max_us", 0)
    fallbacks = during(before, stats, "level_fallbacks")
    return {
        "test": "b-lat",
        "passed": 0 < worst <= cfg.latency_budget_us,
        "worst_service_us": worst,
        "budget_us": cfg.latency_budget_us,
        # Fallbacks lack a fresh edge and are excluded from latency measurement.
        "level_fallbacks": fallbacks,
    }


def case_heap(dev, cfg):
    """Check zero heap growth after session construction (AR-1)."""
    interval_us = round(1_000_000 / cfg.rate_hz)
    isolate(dev, cfg.sensor)
    enable(dev, cfg.sensor, interval_us)
    dev.collect_run(cfg.duration * 1000)
    dev.send("heap")
    line = dev.wait_for("# heap", 2.0) or ""
    fields = {}
    for token in line[len("# heap"):].split():
        key, _, value = token.partition("=")
        if value.isdigit():
            fields[key] = int(value)
    growth = fields.get("growth")
    armed = fields.get("armed", 0) == 1
    return {
        "test": "b-heap",
        "passed": armed and growth == 0,
        "armed": armed,
        "growth_bytes": growth,
    }


def case_parity(dev, cfg):
    """Compare identical streams over both buses in one session (HR-4)."""
    interval_us = round(1_000_000 / cfg.rate_hz)
    result = {"test": "b-parity", "passed": True, "bus": []}
    seen = {}
    for bus in ("i2c", "spi"):
        ready = select_bus(dev, bus)
        result["bus"].append(dev.bus)
        if not ready:
            result["passed"] = False
            result[f"{bus}_hz"] = "no-ready"
            continue
        isolate(dev, cfg.sensor)
        enable(dev, cfg.sensor, interval_us)
        before = snapshot(dev)
        events, stats = dev.collect_run(cfg.duration * 1000)
        events = only(events, cfg.sensor)
        rate = delivered_rate_hz(events)
        gaps = sequence_gaps(events)
        drops = during(before, stats, "reassembly_drops")
        seen[bus] = rate
        result[f"{bus}_hz"] = round(rate, 1)
        result[f"{bus}_gaps"] = gaps
        if not events or gaps != 0 or drops != 0:
            result["passed"] = False

    if len(seen) == 2:
        faster = max(seen.values())
        slower = min(seen.values())
        # Both buses should deliver the same requested rate within a tenth.
        agree = faster > 0 and (faster - slower) / faster <= 0.1
        result["agree"] = agree
        if not agree:
            result["passed"] = False
    return result


def case_soak(dev, cfg):
    """Run at a working rate with no drops, gaps or resets (OR-1)."""
    interval_us = round(1_000_000 / cfg.rate_hz)
    isolate(dev, cfg.sensor)
    enable(dev, cfg.sensor, interval_us)
    before = snapshot(dev)
    events, stats = dev.collect_run(cfg.soak_duration * 1000)
    events = only(events, cfg.sensor)
    gaps = sequence_gaps(events)
    drops = during(before, stats, "reassembly_drops")
    restarts = during(before, stats, "transport_restarts")
    resets = during(before, stats, "hub_resets")
    return {
        "test": "b-soak",
        "passed": bool(events) and gaps == 0 and drops == 0
                  and restarts == 0 and resets == 0,
        "seconds": cfg.soak_duration,
        "events": len(events),
        "seq_gaps": gaps,
        "reasm_drops": drops,
        "transport_restarts": restarts,
        "hub_resets": resets,
        "delivered_hz": round(delivered_rate_hz(events), 1),
    }


def wait_ready(dev):
    """Wait for READY. Query stats if the port missed the '# ready' transition."""
    if dev.wait_for("# ready", READY_TIMEOUT_S) is not None:
        return True
    dev.send("stat")
    return parse_stats(dev.wait_for("# stats", 2.0) or "").get("ready", 0) == 1


TESTS = {
    "b-boot": case_boot,
    "b-rate": case_rate,
    "b-rv400": case_rv400,
    "b-ts": case_ts,
    "b-rvphase": case_rvphase,
    "b-i2c100": case_i2c100,
    "b-girv": case_girv,
    "b-lat": case_lat,
    "b-heap": case_heap,
    "b-parity": case_parity,
    "b-reset": case_reset,
    "b-soak": case_soak,
}


def main():
    parser = argparse.ArgumentParser(description="pico-bno085 device validation")
    parser.add_argument("--port", help="serial device path or socat PTY "
                        "(see examples/bench/README.md)")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=1.0,
                        help="serial read timeout (s)")
    default_tests = [t for t in ALL_TESTS if t not in SLOW_TESTS]
    parser.add_argument("--tests", default=",".join(default_tests),
                        help="comma-separated subset of " + ",".join(ALL_TESTS)
                             + " (default omits " + ",".join(sorted(SLOW_TESTS))
                             + ")")
    parser.add_argument("--bus", choices=("i2c", "spi"), required=True,
                        help="the session's bus (b-rv400 and b-girv use SPI, "
                             "b-i2c100 I2C and b-parity both, regardless)")
    parser.add_argument("--sensor", type=lambda s: int(s, 16), default=ROTATION_VECTOR,
                        help="SH-2 report id (hex) for rate/ts/reset tests")
    parser.add_argument("--rate-hz", type=float, default=50.0)
    parser.add_argument("--duration", type=float, default=5.0,
                        help="run seconds per test")
    parser.add_argument("--i2c100-hz", type=float, default=100.0,
                        help="per-sensor rate for b-i2c100 (PR-2)")
    parser.add_argument("--i2c100-duration", type=float, default=60.0,
                        help="b-i2c100 run seconds (PR-2 gates on 60)")
    parser.add_argument("--rv400-hz", type=float, default=400.0,
                        help="rotation-vector rate for b-rv400 (PR-1)")
    parser.add_argument("--rv400-duration", type=float, default=60.0,
                        help="b-rv400 run seconds; PR-1 passes only at 60 or more")
    parser.add_argument("--girv-hz", type=float, default=500.0,
                        help="gyro-integrated RV rate for b-girv (PR-4)")
    parser.add_argument("--latency-budget-us", type=float, default=1000.0,
                        help="worst HINTN service gap allowed by b-lat (HR-5)")
    parser.add_argument("--soak-duration", type=float, default=3600.0,
                        help="b-soak run seconds (OR-1)")
    parser.add_argument("--wire", action="store_true",
                        help="record b-rvphase full wire timing; diagnostic only")
    parser.add_argument("--promote", action="store_true",
                        help="write results to the baselines directory")
    # Beside this script, wherever it is run from.
    parser.add_argument("--output-dir",
                        default=str(Path(__file__).resolve().parent / "baselines"))
    cfg = parser.parse_args()

    # A selection typo should fail before hardware is touched.
    selected = [t for t in cfg.tests.split(",") if t]
    unknown = [t for t in selected if t not in TESTS]
    if unknown:
        sys.exit(f"unknown test(s): {','.join(unknown)}; "
                 f"choose from {','.join(ALL_TESTS)}")
    if not selected:
        sys.exit("no tests selected")

    port = cfg.port
    if port is None:
        sys.exit("pass --port with a serial device or a socat PTY bridged to "
                 "python3 -m infra.pico_tooling device console --serial SERIAL "
                 "(see examples/bench/README.md)")

    try:
        dev = Device(port, cfg.baud, cfg.timeout)
    except OSError as exc:
        # serial.SerialException is an OSError; so is a missing COM port.
        sys.exit(f"cannot open {port}: {exc}")
    # The firmware starts without a bus selected.
    if not start_session(dev, cfg.bus):
        print(f"warning: the device never reached READY on {cfg.bus}; is the "
              "bench firmware flashed and the BNO085 wired?")

    session_bus = dev.bus
    results = []
    for name in selected:
        result = TESTS[name](dev, cfg)
        # Restore the session bus after cases that select their own so each result
        # records the bus actually used.
        result.setdefault("bus", dev.bus)
        results.append(result)
        if name in OWN_BUS and not select_bus(dev, cfg.bus):
            # A bus-selection failure would send later cases to the wrong bus.
            results.append({"test": "reselect", "passed": False,
                            "bus": dev.bus, "after": name})
            break

    print(f"{'test':10} {'result':6}  detail")
    all_passed = True
    skip = {"test", "passed"}
    for result in results:
        all_passed = all_passed and result["passed"]
        detail = " ".join(f"{k}={v}" for k, v in result.items() if k not in skip)
        verdict = "PASS" if result["passed"] else "FAIL"
        print(f"{result['test']:10} {verdict:6}  {detail}")

    if cfg.promote:
        # UTC keeps baseline filenames ordered across time zones.
        now = datetime.datetime.now(datetime.UTC)
        path = Path(cfg.output_dir) / f"bench-{now:%Y%m%d-%H%M%S}.json"
        # Keep run conditions with the measured values in the baseline.
        document = {
            "timestamp": now.isoformat(timespec="seconds"),
            "port": cfg.port,
            "bus": session_bus,
            "info": read_info(dev),
            "results": results,
        }
        with path.open("w", encoding="ascii") as handle:
            json.dump(document, handle, indent=2)
        print(f"wrote {path}")

    sys.exit(0 if all_passed else 1)


if __name__ == "__main__":
    main()
