#!/usr/bin/env python3
"""Run installed JBenchmark packages and capture their result screens.

JAR indices are discovered automatically; --indices can override discovery.
Screenshots pause the VM, so captures happen only after the timed workload.
Inspect every image before recording a score; a waiting screen is not a result.
"""
import argparse
import contextlib
from pathlib import Path
import re

from serial_console import Console, screenshot


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyUSB1")
    parser.add_argument("--indices", type=int, nargs=4,
                        metavar=("JB1", "JB2", "JB3D", "JBHD"),
                        help="Override automatic discovery from the current JAR list")
    parser.add_argument("--scroll-pages", type=int, default=0,
                        help="Capture additional result pages, seven DOWN presses per page")
    parser.add_argument("--suites", nargs="+", choices=("1", "2", "3d", "hd"),
                        default=["1", "2", "3d", "hd"])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 0 <= args.scroll_pages <= 10:
        parser.error("--scroll-pages must be between 0 and 10")
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
        if args.indices is None:
            c.send("list")
            listing = c.until("GAMES_END", 20)
            packages = {Path(m[2]).name.lower().replace(" ", ""): int(m[1])
                        for line in listing if (m := re.match(r"(\d+) (.+?\.jar)(?:\s|$)", line))}
            args.indices = []
            for suite, name in zip(("1", "2", "3d", "hd"),
                                   ("jbenchmark1.jar", "jbenchmark2.jar", "jbenchmark3d.jar", "jbenchmarkhd.jar")):
                if suite in args.suites and name not in packages:
                    raise RuntimeError(f"Missing {name}; install it or pass --indices")
                args.indices.append(packages.get(name, -1))
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
                        for page in range(1, args.scroll_pages + 1):
                            for _ in range(7):
                                c.send("key down down")
                                c.send("key down up")
                                c.until("KEY_OK", 30)
                                c.until("KEY_OK", 30)
                                c.drain(0.1)
                            c.drain(0.4)
                            screenshot(c, args.output / f"jbenchmark{suite}-result-page{page}.png",
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
