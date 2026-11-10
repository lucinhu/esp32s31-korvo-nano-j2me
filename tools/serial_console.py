#!/usr/bin/env python3
"""Board console and CRC-checked JAR uploader (pyserial; Pillow for shots)."""
import argparse
import pathlib
import re
import sys
import time
import zlib

import serial


ANSI = re.compile(r"\x1b\[[0-9;]*m")


class MonitorSerial(serial.Serial):
    # DTR/RTS are wired to BOOT/EN. Updating them one at a time can reset the
    # board even when both requested levels are the same. Preserve the lines.
    def _update_dtr_state(self):
        pass

    def _update_rts_state(self):
        pass


class Console:
    def __init__(self, port, baud):
        self.serial = MonitorSerial(port, baud, timeout=0.2)
        if sys.platform.startswith("linux"):
            # Keep modem control lines stable when the console is reopened.
            import termios
            attrs = termios.tcgetattr(self.serial.fileno())
            attrs[2] &= ~termios.HUPCL
            termios.tcsetattr(self.serial.fileno(), termios.TCSANOW, attrs)
        self.buffer = bytearray()

    def send(self, command):
        self.serial.write((command + "\n").encode())
        self.serial.flush()

    def line(self, deadline):
        while time.monotonic() < deadline:
            if b"\n" in self.buffer:
                raw, _, self.buffer = self.buffer.partition(b"\n")
                return ANSI.sub("", raw.decode(errors="replace")).strip()
            self.buffer.extend(self.serial.read(self.serial.in_waiting or 1))
        raise TimeoutError("No complete console response before deadline")

    def until(self, marker, seconds=30):
        deadline = time.monotonic() + seconds
        lines = []
        while time.monotonic() < deadline:
            line = self.line(deadline)
            print(line, flush=True)
            lines.append(line)
            if "Guru Meditation" in line:
                raise RuntimeError(line)
            if marker in line:
                return lines
        raise TimeoutError(marker)

    def drain(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                print(self.line(end), flush=True)
            except TimeoutError:
                break


def screenshot(console, output, game, transfer_baud=None, full=False):
    from PIL import Image

    original_baud = console.serial.baudrate
    try:
        try:
            w, h, pixels = _screenshot(console, game, transfer_baud, full)
        except (ValueError, TimeoutError, RuntimeError) as error:
            if not transfer_baud or transfer_baud == original_baud:
                raise
            if isinstance(error, RuntimeError) and not str(error).startswith("SHOT_ERROR"):
                raise
            # Some USB-UART bridges still lose bytes at 2 Mbaud. Let the
            # board's pending row/handshake timeout restore its base rate.
            console.serial.baudrate = original_baud
            time.sleep(3.2)
            console.buffer.clear()
            console.serial.reset_input_buffer()
            console.send("")
            console.drain(0.3)
            print(f"Fast screenshot failed ({error}); retrying at {original_baud} baud", flush=True)
            w, h, pixels = _screenshot(console, game, None, full)
    finally:
        changed = console.serial.baudrate != original_baud
        console.serial.baudrate = original_baud
        if changed:
            console.buffer.clear()
            console.serial.reset_input_buffer()
    pathlib.Path(output).parent.mkdir(parents=True, exist_ok=True)
    Image.frombytes("RGB", (w, h), bytes(pixels)).save(output)
    print(f"Saved {output} ({w}x{h})")


def _screenshot(console, game, transfer_baud, full):
    command = "shot game" if game else "shot full" if full else "shot lcd"
    if transfer_baud: command += f" {transfer_baud}"
    console.send(command)
    deadline = time.monotonic() + 90
    acknowledged = False
    while True:
        line = console.line(deadline)
        if line.startswith("SHOT_ERROR"):
            raise RuntimeError(line)
        # A MIDlet may leave a partial log line before the protocol marker.
        if match := re.search(r"SHOT_READY (\d+)( ACK)?$", line):
            acknowledged = bool(match[2])
            console.serial.baudrate = int(match[1])
            console.send("SHOT_GO")
            continue
        if match := re.search(r"SHOT_BEGIN (\d+) (\d+)$", line):
            w, h = int(match[1]), int(match[2])
            # A full 800x480 text-encoded RGB565 frame needs ~134 seconds
            # at the base rate. Keep timeout proportional to wire size.
            deadline = time.monotonic() + max(30, w * h * 4 * 10 / console.serial.baudrate + 30)
            break
        print(line, flush=True)
    pixels = bytearray()
    for _ in range(h):
        row = console.line(deadline)
        # The audio task remains live during capture. Its diagnostics may
        # appear between complete pixel rows; retain them in the log without
        # treating them as image data or acknowledging a row not yet received.
        while re.match(r"^(?:[IWEVD] \(\d+\)|MIDI_[A-Z_]+\b)", row):
            print(row, flush=True)
            row = console.line(deadline)
        if len(row) != w * 4 or not re.fullmatch("[0-9a-f]+", row):
            raise ValueError(f"Invalid screenshot row length={len(row)} expected={w*4}: {row[:100]}")
        for i in range(w):
            v = int(row[i * 4:i * 4 + 4], 16)
            pixels.extend((((v >> 11) & 31) * 255 // 31,
                           ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
        if acknowledged:
            console.send("SHOT_ACK")
    console.until("SHOT_END", 10)
    return w, h, pixels


def upload(console, filename, name, transfer_baud=115200):
    data = pathlib.Path(filename).read_bytes()
    target = name or pathlib.Path(filename).name
    if (not target.lower().endswith(".jar") or ".." in target or
            re.search(r'[\s\\/:*?"<>|\x00-\x1f\x7f]', target) or len(target.encode("utf-8")) > 120):
        raise ValueError("Use a .jar basename without spaces or path characters, at most 120 UTF-8 bytes")
    console.send(f"put {len(data)} {zlib.crc32(data):08x} {target} {transfer_baud}")
    lines = console.until("PUT_", 10)
    ready = re.fullmatch(r"PUT_READY(?: (\d+))?", lines[-1])
    if not ready:
        raise RuntimeError(lines[-1])
    original_baud = console.serial.baudrate
    started = time.monotonic()
    try:
        # Old firmware returns plain PUT_READY and continues at console speed.
        console.serial.baudrate = int(ready[1]) if ready[1] else original_baud
        for offset in range(0, len(data), 1024):
            chunk = data[offset:offset + 1024]
            console.serial.write(chunk)
            console.serial.flush()
            console.until(f"PUT_ACK {offset + len(chunk)}", 10)
        result = console.until("PUT_", 10)[-1]
        if not result.startswith("PUT_OK "):
            raise RuntimeError(result)
    finally:
        changed = console.serial.baudrate != original_baud
        console.serial.baudrate = original_baud
        if changed:
            console.buffer.clear()
            console.serial.reset_input_buffer()
    elapsed = time.monotonic() - started
    print(f"UPLOAD_STATS bytes={len(data)} seconds={elapsed:.3f} KiB_s={len(data)/1024/elapsed:.1f}", flush=True)


def smoke(console):
    # Includes constructor/startApp, scheduler, paint, RMS and full teardown.
    endings = []
    for _ in range(3):
        console.send("demo 90")
        lines = console.until("RUN_END", 45)
        end = lines[-1]
        if "rc=0" not in end or not re.search(r"presents=[1-9]\d*", end):
            raise AssertionError(end)
        endings.append(int(re.search(r"psram_after=(\d+)", end)[1]))
        time.sleep(0.3)
    if max(endings[1:]) != min(endings[1:]):
        raise AssertionError(f"PSRAM drift after repeated teardown: {endings}")
    console.send("demo")
    console.until("DEMO_RMS_", 20)
    time.sleep(0.5)
    for command, expected in [("key fire down", "DEMO_KEY_DOWN=-5"),
                              ("key fire up", "DEMO_KEY_UP=-5"),
                              ("key 5 down", "DEMO_KEY_DOWN=53"),
                              ("key 5 up", "DEMO_KEY_UP=53"),
                              ("pointer down 100 140", "DEMO_POINTER=100,140")]:
        console.send(command)
        console.until(expected, 15)
        time.sleep(0.15)
    console.send("pointer up 100 140")
    console.send("stats")
    console.until("AUDIO writes=", 10)
    console.send("stop")
    console.until("RUN_END", 20)
    print(f"SMOKE_PASS psram_after={endings}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200)
    subs = parser.add_subparsers(dest="action", required=True)
    cmd = subs.add_parser("command")
    cmd.add_argument("text")
    cmd.add_argument("--wait", type=float, default=3)
    shot = subs.add_parser("shot")
    shot.add_argument("output")
    shot_modes = shot.add_mutually_exclusive_group()
    shot_modes.add_argument("--game", action="store_true")
    shot_modes.add_argument("--full", action="store_true", help="Capture the entire 800x480 LCD including controls and FPS")
    shot.add_argument("--fast", action="store_true", help="Capture at 2000000 baud and restore console speed")
    put = subs.add_parser("upload")
    put.add_argument("file")
    put.add_argument("--name")
    put.add_argument("--fast", action="store_true", help="Use 2000000 baud for the upload, then restore console speed")
    subs.add_parser("smoke")
    args = parser.parse_args()
    console = Console(args.port, args.baud)
    try:
        if args.action == "command":
            console.send(args.text)
            console.drain(args.wait)
        elif args.action == "shot":
            screenshot(console, args.output, args.game, 2000000 if args.fast else None, args.full)
        elif args.action == "upload":
            upload(console, args.file, args.name, 2000000 if args.fast else args.baud)
        else:
            smoke(console)
    finally:
        console.serial.close()


if __name__ == "__main__":
    main()
