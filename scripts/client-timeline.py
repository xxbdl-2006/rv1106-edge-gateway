#!/usr/bin/env python3
"""Timestamp ffmpeg's own RTSP progress so the startup wait can be split up.

"Time to first picture" is the number a viewer feels, but on its own it does
not say who is responsible. ffmpeg logs each RTSP step (OPTIONS, DESCRIBE,
SETUP, PLAY) and then the first decoded frame, all on stderr with no clock.
This runs the same pull as frame-arrival.py but stamps every stderr line and
every frame arrival against one clock, so the wait decomposes into:

    spawn -> handshake done   (client + server round trips)
    handshake -> first frame  (waiting for the encoder's next IDR)

Only the second part is something the gateway controls (`-g`), which is the
whole point of separating them.

Usage: client-timeline.py <rtsp-url> [extra ffmpeg args...]
"""

import subprocess
import sys
import threading
import time

FRAME_BYTES = 1280 * 720 * 3 // 2
MAX_FRAMES = 20

events = []
lock = threading.Lock()
started = None


def stamp(kind, text):
    with lock:
        events.append((time.perf_counter() - started, kind, text))


def read_stderr(pipe):
    for raw in iter(pipe.readline, b""):
        stamp("log", raw.decode("utf-8", "replace").rstrip())
    pipe.close()


def main() -> int:
    global started
    if len(sys.argv) < 2:
        print(__doc__)
        return 2

    command = [
        "ffmpeg", "-hide_banner", "-loglevel", "verbose",
        "-rtsp_transport", "tcp",
        *sys.argv[2:],
        "-i", sys.argv[1],
        "-an", "-f", "rawvideo", "-pix_fmt", "yuv420p", "-",
    ]

    started = time.perf_counter()
    process = subprocess.Popen(command, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    assert process.stdout is not None and process.stderr is not None

    thread = threading.Thread(target=read_stderr, args=(process.stderr,))
    thread.daemon = True
    thread.start()

    frames = 0
    while frames < MAX_FRAMES:
        chunk = process.stdout.read(FRAME_BYTES)
        if not chunk or len(chunk) < FRAME_BYTES:
            break
        frames += 1
        stamp("frame", "decoded frame %d" % frames)

    process.stdout.close()
    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
    time.sleep(0.2)

    interesting = ("OPTIONS", "DESCRIBE", "SETUP", "PLAY", "Sending", "Receiving",
                   "method", "Stream #", "Opening", "frame", "session",
                   "Input #", "Duration", "codec")
    for offset, kind, text in sorted(events):
        if kind == "frame" or any(token.lower() in text.lower() for token in interesting):
            print("%7.3f  %-5s %s" % (offset, kind, text))
    return 0


if __name__ == "__main__":
    sys.exit(main())
