#!/usr/bin/env python3
"""Run installed JBenchmark packages and capture their result screens.

Run `list` in serial_console.py first and pass the four current game indices.
Screenshots pause the VM, so captures happen only after the timed workload.
Inspect every image before recording a score; a waiting screen is not a result.
"""
import argparse
import contextlib
from pathlib import Path

from serial_console import Console, screenshot


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyUSB1")
    parser.add_argument("--indices", type=int, nargs=4, required=True,
                        metavar=("JB1", "JB2", "JB3D", "JBHD"))
    parser.add_argument("--suites", nargs="+", choices=("1", "2", "3d", "hd"),
                        default=["1", "2", "3d", "hd"])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    c = Console(args.port, 115200)
    try:
        c.send("stats")
        status = c.until("AUDIO_DETAIL", 20)
        if any("running=1" in line for line in status):
            c.send("stop")
            c.until("RUN_END", 120)
            c.until("AUDIO_DETAIL", 20)
        c.send("settings")
        c.until("SETTINGS", 20)
        for suite, index, seconds in zip(("1", "2", "3d", "hd"), args.indices,
                                         (60, 75, 145, 170)):
            if suite not in args.suites:
                continue
            print(f"Starting JBenchmark {suite}, index {index}", flush=True)
            with (args.output / f"jbenchmark{suite}-final.log").open("w") as log:
                with contextlib.redirect_stdout(log):
                    c.send(f"bench {index} 0")
                    c.until("RUN_BEGIN", 20)
                    c.drain(1)
                    if suite in ("2", "3d"):
                        c.send("key softl down")
                        c.send("key softl up")
                        c.until("KEY_OK", 120)
                        c.until("KEY_OK", 120)
                    c.drain(seconds)
                    c.send("stats")
                    status = c.until("AUDIO_DETAIL", 120)
                    running = any("running=1" in line for line in status)
                    if running:
                        screenshot(c, args.output / f"jbenchmark{suite}-result.png",
                                   True, 2000000)
                        c.send("stop")
                        c.until("RUN_END", 120)
                        c.until("AUDIO_DETAIL", 20)
                    else:
                        print("BENCHMARK_FAILED: MIDlet exited before capture")
            print(f"Captured JBenchmark {suite}; inspect its image and log", flush=True)
    finally:
        c.serial.close()


if __name__ == "__main__":
    main()
