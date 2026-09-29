#!/usr/bin/env python3
"""Stamp the arrival time of every decoded frame and report the timing.

Why this exists: "latency" for a live stream is two different numbers and they
were being conflated.

  * time to first picture  - how long a viewer stares at nothing after asking
                             for the stream. Bounded by the GOP: the encoder
                             only starts a new viewer at an IDR, so the worst
                             case is one GOP length. This one the gateway can
                             tune (`-g`).
  * steady-state spacing   - how evenly frames keep arriving once they do.
                             Drift here means something is buffering and the
                             delay is growing, which is worse than a fixed
                             offset.

Both need per-frame wall-clock stamps, which ffmpeg's own stats line does not
give (it prints stream time, not arrival time). So ffmpeg is asked for
rawvideo on stdout and this reads it a frame at a time.

It measures the client application's view only. It cannot see the gateway's
internal capture-to-send delay, and it cannot see a display.

Usage:
    frame-arrival.py <rtsp-url> [frames] [extra ffmpeg args...]

The first frames are thrown away for the interval statistics. ffmpeg hands
over whatever it buffered during the handshake as one burst -- the giveaway is
a sub-millisecond interval next to a 70 ms one -- and averaging that in skews
the spacing. The raw rate is reported alongside so the window can be sanity
checked: at 30 fps, 300 frames must span about 10 s.
"""

import json
import statistics
import subprocess
import sys
import time

WIDTH = 1280
HEIGHT = 720
FRAME_BYTES = WIDTH * HEIGHT * 3 // 2

# Frames to ignore before judging spacing. The burst above lands in here.
SETTLE_FRAMES = 30


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    url = sys.argv[1]
    frames_wanted = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    extra = sys.argv[3:]

    command = [
        "ffmpeg", "-hide_banner", "-loglevel", "error",
        "-rtsp_transport", "tcp",
        *extra,
        "-i", url,
        "-an",
        "-f", "rawvideo", "-pix_fmt", "yuv420p",
        "-",
    ]

    started = time.perf_counter()
    process = subprocess.Popen(command, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL)
    if process.stdout is None:
        print("no stdout from ffmpeg")
        return 1

    arrivals = []
    try:
        while len(arrivals) < frames_wanted:
            chunk = process.stdout.read(FRAME_BYTES)
            if not chunk or len(chunk) < FRAME_BYTES:
                break
            arrivals.append(time.perf_counter() - started)
    finally:
        process.stdout.close()
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()

    if not arrivals:
        print(json.dumps({"error": "no frames at all"}))
        return 1

    # Time to first picture is the whole point for short windows, so report it
    # even when there are not enough frames left to say anything about spacing.
    if len(arrivals) < SETTLE_FRAMES + 10:
        print(json.dumps({
            "frames": len(arrivals),
            "time_to_first_frame_s": round(arrivals[0], 4),
            "note": "too few frames for interval statistics",
        }))
        return 0

    steady = arrivals[SETTLE_FRAMES:]
    intervals = [b - a for a, b in zip(steady, steady[1:])]
    ordered = sorted(intervals)

    def pct(p):
        index = min(len(ordered) - 1, max(0, int(round(p / 100.0 * len(ordered))) - 1))
        return ordered[index]

    span = steady[-1] - steady[0]
    report = {
        "frames": len(arrivals),
        "time_to_first_frame_s": round(arrivals[0], 4),
        "steady_window_s": round(span, 4),
        "steady_rate_fps": round((len(steady) - 1) / span, 3),
        "interval_ms_mean": round(statistics.mean(intervals) * 1000.0, 3),
        "interval_ms_p50": round(statistics.median(intervals) * 1000.0, 3),
        "interval_ms_p95": round(pct(95) * 1000.0, 3),
        "interval_ms_max": round(max(intervals) * 1000.0, 3),
        "interval_ms_min": round(min(intervals) * 1000.0, 3),
        "jitter_ms_stdev": round(statistics.pstdev(intervals) * 1000.0, 3),
        "drift_ms_tail_minus_head": round(
            (statistics.mean(intervals[-25:]) - statistics.mean(intervals[:25])) * 1000.0, 3
        ),
    }
    print(json.dumps(report))
    return 0


if __name__ == "__main__":
    sys.exit(main())
