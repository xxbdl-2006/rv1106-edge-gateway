#!/usr/bin/env python3
"""Sample the time to first picture and report its distribution.

The wait has two parts and the distribution tells them apart:

  * a floor that every connection pays - process start, RTSP handshake, and
    ffmpeg reading enough to describe the stream;
  * a wait for the encoder's next IDR, which the gateway gates a joining
    viewer on (see the `joining` check in src/rtsp_server.c). The keyframe
    interval measured on this board is exactly 1.000 s, so this part should be
    uniform over 0..1 s.

So: if the spread is about 1 s, the lottery is real and the floor is the
minimum. If the spread is small, the IDR wait is not the dominant term and
something else is.

The start of each connection is deliberately randomised. Sampling
back-to-back is not the same as sampling independently here: if the whole
cycle happens to be a multiple of the keyframe interval, every run lands on
the same phase and a uniform 0..1000 ms wait shows up as a 76 ms spread.

Usage: first-frame-stats.py <rtsp-url> [samples] [extra ffmpeg args...]
"""

import json
import os
import random
import statistics
import subprocess
import sys
import time

URL = sys.argv[1] if len(sys.argv) > 1 else "rtsp://172.32.0.93:8554/live/0"
SAMPLES = int(sys.argv[2]) if len(sys.argv) > 2 else 20
EXTRA = sys.argv[3:]
HERE = os.path.dirname(os.path.abspath(__file__))

values = []
for index in range(SAMPLES):
    # Randomise the moment this connection starts, relative to the stream's
    # keyframe cadence. Without this the samples are not independent: the whole
    # cycle (spawn, handshake, wait for a keyframe, a few frames, teardown) can
    # happen to be an exact multiple of the keyframe interval, in which case
    # every run lands on the same phase and the wait looks deterministic. The
    # first run of this measurement did exactly that and reported a 76 ms
    # spread for what should be a uniform 0..1000 ms.
    if index > 0:
        time.sleep(random.uniform(0.0, 1.5))

    result = subprocess.run(
        [sys.executable, os.path.join(HERE, "frame-arrival.py"), URL, "5", *EXTRA],
        capture_output=True, text=True)
    try:
        values.append(json.loads(result.stdout)["time_to_first_frame_s"])
    except Exception:
        pass

for index, value in enumerate(values, 1):
    print("  sample %2d: %.3f s" % (index, value))

print()
if values:
    print("n=%d  min=%.3f  median=%.3f  mean=%.3f  max=%.3f  spread=%.3f" % (
        len(values), min(values), statistics.median(values),
        statistics.mean(values), max(values), max(values) - min(values)))
    print("expected: IDR wait uniform over 0..1.000 s (measured keyframe"
          " interval is exactly 1.000 s)")
