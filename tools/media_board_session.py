#!/usr/bin/env python3
"""Keep one exclusive serial connection across flashing and board verification.

Run with the IDF Python environment. Accepts one JSON action per stdin line:
command, until, run, launch, smoke, upload, screenshot, sample, flash, exit.
"""
import argparse
import fcntl
import hashlib
import json
from pathlib import Path
import re
import statistics
import struct
import sys
import termios
import time
import traceback

import esptool
from esptool.targets.esp32s31 import ESP32S31ROM

from serial_console import Console, screenshot, smoke, upload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyUSB0")
    parser.add_argument("--flash", type=Path)
    parser.add_argument("--diff-with", type=Path, help="Previous app image; esptool verifies the complete result")
    args = parser.parse_args()
    for attempt in range(60):
        try:
            loader = ESP32S31ROM(args.port)
            break
        except esptool.util.FatalError:
            if attempt == 59:
                raise
            print("BOARD_PORT_BUSY waiting for the current serial operation", flush=True)
            time.sleep(3)
    serial = loader._port
    # Unlike advisory flock, TIOCEXCL also excludes non-cooperating consoles.
    # The same descriptor remains open until all checks have finished.
    fcntl.ioctl(serial.fileno(), termios.TIOCEXCL)
    attrs = termios.tcgetattr(serial.fileno())
    attrs[2] &= ~termios.HUPCL
    termios.tcsetattr(serial.fileno(), termios.TCSANOW, attrs)
    serial.timeout = .2
    console = Console.__new__(Console)
    console.serial = serial
    console.buffer = bytearray()

    def flash(path, previous=None):
        path = Path(path).resolve()
        data = path.read_bytes()
        if len(data) > 0x800000 or len(data) < 32 or data[0] != 0xE9:
            raise ValueError("Expected an ESP app image fitting the 8 MiB app partition")
        print("BOARD_FLASH_SHA256", hashlib.sha256(data).hexdigest(), flush=True)
        rom = ESP32S31ROM(serial)
        rom.connect()
        command = ["--chip", "esp32s31", "--baud", "2000000", "--no-stub",
                      "--after", "no-reset", "write-flash", "--flash-mode", "dio",
                      "--flash-freq", "80m", "--flash-size", "16MB", "--no-progress",
                      "0x10000", str(path)]
        if previous:
            command.extend(["--diff-with", str(Path(previous).resolve())])
        esptool.main(command, esp=rom)
        offset = 24
        for index in range(data[1]):
            actual = esptool.cmds.read_flash(rom, 0x10000 + offset, 8, no_progress=True)
            if actual != data[offset:offset + 8]:
                raise RuntimeError(f"Segment {index} header readback differs")
            _, length = struct.unpack_from("<II", data, offset)
            offset += 8 + length
        print("BOARD_FLASH_SEGMENTS_VERIFIED", data[1], flush=True)
        console.buffer.clear()
        rom.hard_reset()
        serial.baudrate = 115200
        console.until("READY nano_j2me", 30)
        console.drain(.5)

    try:
        if args.flash:
            try:
                flash(args.flash, args.diff_with)
            except Exception:
                traceback.print_exc()
        print("BOARD_SESSION_READY", flush=True)
        for line in sys.stdin:
            try:
                action = json.loads(line)
                kind = action["action"]
                if kind == "exit":
                    break
                if kind == "command":
                    console.send(action["text"])
                    console.drain(action.get("wait", 3))
                elif kind == "launch":
                    index = int(action["index"])
                    console.send("stats")
                    status = console.until("STATUS ", 10)[-1]
                    console.until("AUDIO_DETAIL", 10)
                    if "running=1" in status:
                        console.send("stop")
                        console.until("RUN_END", 120)
                        console.until("AUDIO_DETAIL", 10)
                    console.send(f"bench {index} 0")
                    line = console.until("RUN_BEGIN", 10)[-1]
                    if not re.search(rf"RUN_BEGIN index={index}(?: |$)", line):
                        raise RuntimeError("A different game started during launch: " + line)
                elif kind == "until":
                    console.until(action["marker"], action.get("seconds", 30))
                elif kind == "run":
                    console.send(action["text"])
                    console.until(action["marker"], action.get("seconds", 30))
                elif kind == "smoke":
                    smoke(console)
                elif kind == "upload":
                    upload(console, action["file"], action.get("name"), 2000000)
                elif kind == "screenshot":
                    screenshot(console, action["file"], action.get("game", True), 2000000,
                               action.get("full", False))
                elif kind == "sample":
                    count = max(1, min(120, int(action.get("count", 15))))
                    interval = max(.2, float(action.get("interval", 1)))
                    result = {"label": action.get("label", ""), "samples": []}
                    console.send("stats")
                    result["before"] = console.until("AUDIO_DETAIL", 10)
                    for _ in range(count):
                        console.send("fps")
                        line = console.until("FPS_STATE", 10)[-1]
                        match = re.search(r"rate=([0-9.]+)", line)
                        if not match:
                            raise RuntimeError("FPS response lacks a rate: " + line)
                        result["samples"].append(float(match[1]))
                        console.drain(interval)
                    console.send("stats")
                    result["after"] = console.until("AUDIO_DETAIL", 10)
                    values = result["samples"]
                    result.update(mean=statistics.mean(values), median=statistics.median(values),
                                  minimum=min(values), maximum=max(values))
                    target = Path(action["file"])
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n")
                    print("BOARD_FPS_RESULT", json.dumps(result, ensure_ascii=False), flush=True)
                elif kind == "flash":
                    flash(action["file"], action.get("previous"))
                else:
                    raise ValueError(f"Unknown action: {kind}")
                print("BOARD_ACTION_OK", kind, flush=True)
            except Exception:
                traceback.print_exc()
                print("BOARD_ACTION_ERROR", flush=True)
            print("BOARD_SESSION_READY", flush=True)
    finally:
        try:
            fcntl.ioctl(serial.fileno(), termios.TIOCNXCL)
        except OSError:
            pass  # USB may have been disconnected during the session.
        finally:
            serial.close()


if __name__ == "__main__":
    main()
