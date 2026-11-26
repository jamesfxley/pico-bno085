# SPDX-License-Identifier: MIT
"""The parity test: the GUI's stream figures against the bench's own maths.

The expected figures were produced by running the bench harness's functions
on exactly the fixture lines: `delivered_rate_hz` and `timestamp_detail`
over each sensor's whole stream, with the nominal interval this package
derives when no `cfg` was sent (the lower median of the observed
intervals); and `sequence_gaps` applied to each run segment separately,
summed, because the harness's counter reads a jump between two runs as lost
reports while this package's model compares no sequence across a `# ready`
or `# run` boundary. `tests/data/README.md` states the steps in full.

What the comparison does not cover:

- every gap, repeat and inversion figure of the fixture is zero apart from
  the gyro-integrated rotation vector, whose line carries a placeholder
  sequence, so a stub returning zero for those fields passes this file; the
  nonzero paths are covered by `test_stats.py`, by the model tests and by
  the derived case below;
- the nominal interval is the median this package derives, not the
  commanded interval the harness itself would pass, so the comparison
  checks the formulas on identical inputs, not the choice of basis;
- the window is widened to the whole stream, so pruning and restart
  boundaries are not exercised here; the model tests cover them.
"""

from dataclasses import dataclass
from pathlib import Path

from pico_gui_bno085.model import Model
from pico_gui_bno085.monitor import Monitor
from pico_gui_bno085.stats import DEFAULT_WINDOW_US
from pico_gui_bno085.stats import Figures as StreamFigures

DATA = Path(__file__).resolve().parent.parent / "data"
FIXTURE = "bench-excerpt.txt"


@dataclass(frozen=True)
class Figures:
    """The bench's figures for one sensor of the fixture."""

    count: int
    rate_hz: float
    nominal_interval_us: int
    monotonic: bool
    p99_jitter: float
    inversions: int
    worst_backward_us: int
    segment_gaps: int


EXPECTED: dict[int, Figures] = {
    1: Figures(
        count=49,
        rate_hz=3.947175282130521,
        nominal_interval_us=7881,
        monotonic=True,
        p99_jitter=1495.0709300850147,
        inversions=0,
        worst_backward_us=0,
        segment_gaps=0,
    ),
    2: Figures(
        count=38,
        rate_hz=100.12854340031122,
        nominal_interval_us=9952,
        monotonic=True,
        p99_jitter=0.07395498392282958,
        inversions=0,
        worst_backward_us=0,
        segment_gaps=0,
    ),
    3: Figures(
        count=38,
        rate_hz=100.96103995328505,
        nominal_interval_us=9979,
        monotonic=True,
        p99_jitter=0.3502354945385309,
        inversions=0,
        worst_backward_us=0,
        segment_gaps=0,
    ),
    4: Figures(
        count=38,
        rate_hz=100.14480397331276,
        nominal_interval_us=9973,
        monotonic=True,
        p99_jitter=0.0574551288478893,
        inversions=0,
        worst_backward_us=0,
        segment_gaps=0,
    ),
    5: Figures(
        count=137,
        rate_hz=8.795904109640427,
        nominal_interval_us=10000,
        monotonic=True,
        p99_jitter=381.5165,
        inversions=0,
        worst_backward_us=0,
        segment_gaps=0,
    ),
    6: Figures(
        count=38,
        rate_hz=100.12773051027257,
        nominal_interval_us=9981,
        monotonic=True,
        p99_jitter=0.05650736399158401,
        inversions=0,
        worst_backward_us=0,
        segment_gaps=0,
    ),
    7: Figures(
        count=45,
        rate_hz=100.26547563435004,
        nominal_interval_us=9956,
        monotonic=True,
        p99_jitter=0.06167135395741261,
        inversions=0,
        worst_backward_us=0,
        segment_gaps=0,
    ),
    8: Figures(
        count=50,
        rate_hz=100.1035765577853,
        nominal_interval_us=10020,
        monotonic=True,
        p99_jitter=0.025449101796407185,
        inversions=0,
        worst_backward_us=0,
        segment_gaps=0,
    ),
    9: Figures(
        count=22,
        rate_hz=50.0479029928646,
        nominal_interval_us=20017,
        monotonic=True,
        p99_jitter=0.010091422291052605,
        inversions=0,
        worst_backward_us=0,
        segment_gaps=0,
    ),
    15: Figures(
        count=44,
        rate_hz=100.12690503079484,
        nominal_interval_us=9978,
        monotonic=True,
        p99_jitter=0.022850270595309682,
        inversions=0,
        worst_backward_us=0,
        segment_gaps=0,
    ),
    40: Figures(
        count=45,
        rate_hz=100.22162646033159,
        nominal_interval_us=9976,
        monotonic=True,
        p99_jitter=0.04019647153167602,
        inversions=0,
        worst_backward_us=0,
        segment_gaps=0,
    ),
    41: Figures(
        count=44,
        rate_hz=100.13459953146322,
        nominal_interval_us=9972,
        monotonic=True,
        p99_jitter=0.05094263939029282,
        inversions=0,
        worst_backward_us=0,
        segment_gaps=0,
    ),
    42: Figures(
        count=292,
        rate_hz=18.784880327401744,
        nominal_interval_us=1051,
        monotonic=True,
        p99_jitter=3.9077069457659372,
        inversions=0,
        worst_backward_us=0,
        segment_gaps=73950,
    ),
}


def read_lines(fixture: str) -> list[str]:
    """Return the fixture's lines, as the session would read them."""
    return (DATA / fixture).read_text(encoding="ascii").splitlines()


def span_window(lines: list[str]) -> int:
    """Return a window wide enough to keep every telemetry line."""
    times = [int(line.split(",")[3]) for line in lines if line and line[1] == ","]
    return max(times, default=0) + DEFAULT_WINDOW_US


def figures_of_fixture(lines: list[str]) -> dict[int, StreamFigures]:
    """Feed the lines through the monitor and return the per-sensor figures."""
    monitor = Monitor(Model(window_us=span_window(lines)))
    for line in lines:
        monitor.on_line(line, 0.0)
    return {stats.sensor: stats.figures for stats in monitor.snapshot().sensors}


def test_the_gui_reproduces_the_bench_figures() -> None:
    lines = read_lines(FIXTURE)
    figures = figures_of_fixture(lines)
    assert set(figures) == set(EXPECTED)
    for sensor, bench in EXPECTED.items():
        of_sensor = figures[sensor]
        assert of_sensor.count == bench.count
        assert of_sensor.rate_hz == bench.rate_hz
        assert of_sensor.nominal_interval_us == bench.nominal_interval_us
        assert not of_sensor.nominal_is_commanded
        timestamps = of_sensor.timestamps
        assert timestamps.monotonic is bench.monotonic
        assert timestamps.inversions == bench.inversions
        assert timestamps.worst_backward_us == bench.worst_backward_us
        assert timestamps.p99_ratio == bench.p99_jitter
        if sensor != 0x2A:
            assert of_sensor.missing == bench.segment_gaps


def test_hand_edited_lines_count_by_hand() -> None:
    """Missing, repeats, inversions and the worst backward step, by hand.

    The case takes the first six R lines of sensor 8 from the fixture,
    deletes the fourth and repeats the second, so every figure is nonzero
    except the worst backward step and the arithmetic that produces each is
    visible:

      sequences:  s0 s1 s1 s2 s4 s5 — the repeated s1 counts one repeat,
                  and the deleted s3 leaves missing = (s4 - s2 - 1) % 256;
      times:      t0 t1 t1 t2 t4 t5 — the repeated line's equal step is
                  the one inversion, and as the only non-positive step it
                  is also the worst backward step, which is 0: both of its
                  times are the repeated line's.
    """
    lines = [line for line in read_lines(FIXTURE) if line.startswith("R,8,")][:6]
    assert len(lines) == 6
    fields = [line.split(",") for line in lines]
    seqs = [int(field[2]) for field in fields]
    edited = [lines[0], lines[1], lines[1], lines[2], lines[4], lines[5]]
    monitor = Monitor(Model(window_us=span_window(edited)))
    for line in edited:
        monitor.on_line(line, 0.0)
    figures = monitor.snapshot().sensors[0].figures
    assert figures.missing == (seqs[4] - seqs[2] - 1) % 256
    assert figures.repeats == 1
    assert figures.timestamps.inversions == 1
    assert figures.timestamps.worst_backward_us == 0
    assert figures.timestamps.monotonic is False
