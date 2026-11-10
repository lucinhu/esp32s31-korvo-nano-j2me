#!/usr/bin/env python3
"""Build local, deterministic CPU/render and continuous audio test MIDlets."""
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import zipfile

root = Path(__file__).resolve().parent.parent
out = root / "build/performance/fixtures"
out.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory() as work:
    work = Path(work)
    cp = subprocess.check_output(["python3", str(root / "third_party/nano_j2me/tools/midp_stubs.py"),
                                  "--out", str(work / "stubs"), "--quiet"], text=True).strip()
    classes = work / "classes"
    subprocess.run(["javac", "--release", "8", "-nowarn", "-classpath", cp, "-d", str(classes),
                    str(root / "tests/midlet/PerfBench.java"), str(root / "tests/midlet/AudioBench.java"),
                    str(root / "tests/midlet/RenderBench.java"),
                    str(root / "tests/midlet/RgbBlitBench.java")], check=True)
    rate = 22050
    pcm = b"".join(struct.pack("<h", round(12000 * math.sin(2 * math.pi * 440 * i / rate))) for i in range(rate * 3))
    wav = b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) + b"data" + struct.pack("<I", len(pcm)) + pcm
    # Six half-second notes, tempo 120, quarter-note resolution 96 ticks.
    track = bytearray(b"\x00\xff\x51\x03\x07\xa1\x20\x00\xc0\x00")
    for note in (60, 64, 67, 72, 67, 64):
        track += bytes((0, 0x90, note, 90, 96, 0x80, note, 0))
    track += b"\x00\xff\x2f\x00"
    midi = b"MThd" + struct.pack(">IHHH", 6, 0, 1, 96) + b"MTrk" + struct.pack(">I", len(track)) + track
    for filename, main, extra in [("perf-cpu.jar", "PerfBench", "Perf-Work: 4000\r\nPerf-Frames: 120\r\n"),
                                   ("perf-render2.jar", "RenderBench", ""),
                                   ("perf-rgb-blit.jar", "RgbBlitBench", ""),
                                   ("perf-audio.jar", "AudioBench", "")]:
        with zipfile.ZipFile(out / filename, "w", zipfile.ZIP_DEFLATED) as jar:
            manifest = f"Manifest-Version: 1.0\r\nMIDlet-Name: {main}\r\nMIDlet-1: {main}, , {main}\r\nMicroEdition-Profile: MIDP-2.0\r\nMicroEdition-Configuration: CLDC-1.1\r\n{extra}\r\n"
            jar.writestr("META-INF/MANIFEST.MF", manifest)
            for p in sorted(classes.glob(main + "*.class")):
                jar.writestr(p.name, p.read_bytes())
            if main == "AudioBench":
                jar.writestr("tone.wav", wav)
                jar.writestr("notes.mid", midi)
        print(out / filename)
