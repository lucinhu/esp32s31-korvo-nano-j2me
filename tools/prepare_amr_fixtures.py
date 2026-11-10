#!/usr/bin/env python3
"""Generate audible AMR fixtures from a sine wave and synthesized speech.

Needs ffmpeg, espeak-ng, libopencore-amrnb and libvo-amrwbenc.
The generated fixtures are checked in; normal firmware/JAR builds need no encoder.
"""
import argparse
import array
import ctypes
import ctypes.util
import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import wave

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests/media/fixtures"


def pcm_stats(encoded, rate):
    raw = subprocess.check_output(["ffmpeg", "-v", "error", "-i", "pipe:0",
                                   "-f", "s16le", "pipe:1"], input=encoded)
    samples = array.array("h", raw)
    if sys.byteorder != "little":
        samples.byteswap()
    if not samples:
        raise ValueError("AMR fixture decoded no samples")
    peak = max(map(abs, samples))
    rms = math.sqrt(sum(s * s for s in samples) / len(samples))
    if peak < 2000 or rms < 500:
        raise ValueError(f"AMR fixture is too quiet for an audible test: peak={peak}, rms={rms}")
    return {"rate": rate, "samples": len(samples), "duration_ms": len(samples) * 1000 // rate,
            "peak": peak, "rms": round(rms, 2), "sha256": hashlib.sha256(encoded).hexdigest()}


def encode(raw, library, wide):
    codec = ctypes.CDLL(library)
    prefix = "E_IF" if wide else "Encoder_Interface"
    initialize = getattr(codec, prefix + "_init")
    process = getattr(codec, prefix + ("_encode" if wide else "_Encode"))
    close = getattr(codec, prefix + "_exit")
    initialize.argtypes = [] if wide else [ctypes.c_int]
    initialize.restype = ctypes.c_void_p
    process.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_short),
                        ctypes.POINTER(ctypes.c_ubyte), ctypes.c_int]
    process.restype = ctypes.c_int
    close.argtypes = [ctypes.c_void_p]
    close.restype = None
    state = initialize() if wide else initialize(0)
    if not state:
        raise RuntimeError("AMR encoder initialization failed")
    frames = 320 if wide else 160
    data = bytearray(b"#!AMR-WB\n" if wide else b"#!AMR\n")
    output = (ctypes.c_ubyte * 128)()
    try:
        for offset in range(0, len(raw), frames * 2):
            block = array.array("h", raw[offset:offset + frames * 2].ljust(frames * 2, b"\0"))
            if sys.byteorder != "little":
                block.byteswap()
            pcm = (ctypes.c_short * frames)(*block)
            # Highest speech bitrate, DTX disabled: every frame carries sound.
            size = process(state, 8 if wide else 7, pcm, output, 0 if wide else 1)
            if not 0 < size <= len(output):
                raise RuntimeError(f"AMR encoder returned invalid frame size {size}")
            data.extend(bytes(output[:size]))
    finally:
        close(state)
    return bytes(data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--amrwb-library", help="Path to a locally built libvo-amrwbenc.so")
    args = parser.parse_args()
    narrow = ctypes.util.find_library("opencore-amrnb")
    wide = args.amrwb_library or ctypes.util.find_library("vo-amrwbenc")
    if not narrow or not wide:
        parser.error("Need libopencore-amrnb and libvo-amrwbenc; see tests/media/fixtures/README.md")
    FIXTURES.mkdir(parents=True, exist_ok=True)
    metadata = {}
    with tempfile.TemporaryDirectory() as temporary:
        temporary = Path(temporary)
        tone = temporary / "tone.wav"
        with wave.open(str(tone), "wb") as output:
            output.setparams((1, 2, 22050, 0, "NONE", "not compressed"))
            output.writeframes(b"".join(struct.pack("<h", round(6000 * math.sin(2 * math.pi * 440 * i / 22050)))
                                        for i in range(13230)))
        speech = temporary / "speech.wav"
        subprocess.run(["espeak-ng", "-s", "170", "-a", "80", "-w", str(speech),
                        "Audio test. One, two, three."], check=True)
        (FIXTURES / "speech.wav").write_bytes(speech.read_bytes())
        for name, source in (("tone", tone), ("speech", speech)):
            for is_wide, rate, library, suffix in ((False, 8000, narrow, "amr"),
                                                   (True, 16000, wide, "awb")):
                raw = subprocess.check_output(["ffmpeg", "-v", "error", "-i", str(source),
                    "-ac", "1", "-ar", str(rate), "-f", "s16le", "pipe:1"])
                encoded = encode(raw, library, is_wide)
                filename = f"{name}.{suffix}"
                metadata[filename] = pcm_stats(encoded, rate)
                (FIXTURES / filename).write_bytes(encoded)
                print(filename, metadata[filename])
    (FIXTURES / "signal.json").write_text(json.dumps(metadata, indent=2) + "\n")


if __name__ == "__main__":
    main()
