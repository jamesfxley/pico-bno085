`session.txt` is assembled for the tests from lines that bench sessions on a
board printed, in the bench line format (LP-3 to LP-7). It is not one
continuous session, so its `# done` counts do not match the lines present.

`bench-excerpt.txt` is assembled from the board lines of one recorded check
of the payload mode. It keeps the head of each run's telemetry followed by
that run's closing lines, so each sensor that prints a payload line appears,
together with the `# ready` lines, the three `# cal` responses, the
`# dcd` response, a refused tare's `# err parse` line and the firmware's own
start-up and recovery log lines. The runs are cut short, so the excerpt is
not one continuous session either and its `# done` counts do not match the
lines present. The pass it comes from printed no `# resp` line: the hub never
answered its tare, which the check recorded as its one failed expectation.

The expected figures of `tests/bno085/test_parity.py` were produced as
follows, and running the same steps on this file reproduces them. Take
every telemetry line, E or payload, and read each one's sensor, sequence and
event time from its four common fields. Per sensor, over the sensor's whole
stream in this file:

- the delivered rate is the bench harness's `delivered_rate_hz`, the count
  of intervals over the span between the first and the last event time;
- the timestamp figures are the harness's `timestamp_detail`, given the
  nominal interval this package derives when no `cfg` was sent: the lower
  middle of the sorted observed intervals, a whole number of microseconds;
- the gap figure is the harness's `sequence_gaps`, applied to each run's
  events separately and summed, with a new segment at every `# run` and
  `# ready` line, because the harness's counter reads a jump between two
  runs as lost reports while this package's model compares no sequence
  across such a boundary. The gyro-integrated rotation vector's gap figure
  is not compared: its line carries a placeholder sequence, which the
  harness's counter reads as losses.

The parity test itself runs this package's model over the same lines and
compares the two results. The figures are committed with the test as
constants. The harness is not part of this package, so no code here
recomputes them; a reader reproduces them by the steps above.
