#!/usr/bin/env python3
"""Run fixed-work MIDlets and fixed-frame game samples on the connected board."""
import argparse
import json
from pathlib import Path
import re
import time
from serial_console import Console

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path)
parser.add_argument("--port", default="/dev/ttyUSB0")
parser.add_argument("--only", choices=["cpu", "render", "audio", "kernels", "mixed", "games", "all"], default="all")
parser.add_argument("--mixed-jar", default="perf-mixed.jar", help="Uploaded mixed-kernel JAR basename")
parser.add_argument("--game-keys", default="", help="Optional upstream key script, e.g. 30:fire,90:soft1,150:5")
parser.add_argument("--jit", choices=["on", "off", "probe", "keep"], default="keep", help="Select JIT for the next runs (same-firmware comparison)")
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
c = Console(args.port, 115200)
results = []
try:
    c.drain(1.5)
    c.send("stats"); status = c.until("STATUS ", 10)[-1]; c.until("AUDIO_DETAIL", 10)
    if "running=1" in status:
        c.send("stop"); c.until("RUN_END", 30); c.until("AUDIO_DETAIL", 10)
    c.send("fps off"); c.until("FPS_STATE off", 10)
    c.send("res 240 320"); c.until("RES_OK 240 320", 10)
    if args.jit != "keep":
        c.send(f"jit {args.jit}"); c.until(f"JIT_STATE {args.jit}", 10)
    c.send("volume 40"); c.until("VOLUME 40", 8)
    c.send("list"); listing = c.until("GAMES_END", 10)
    indices = {}
    for line in listing:
        match = re.match(r"(\d+) (\S+\.jar)(?: |$)", line)
        if match: indices[Path(match[2]).name] = int(match[1])
    jobs = []
    for name in ("cpu", "render", "audio"):
        if args.only in ("all", name):
            for repeat in range(2 if name != "audio" else 1):
                filename="perf-render2.jar" if name=="render" and "perf-render2.jar" in indices else f"perf-{name}.jar"
                jobs.append((f"{name}-{repeat}", indices[filename], 0, ""))
    if args.only in ("all", "games"):
        keys = args.game_keys
        jobs += [("nfs14", indices["g77d10a56e51a.jar"], 300, keys), ("nfs16", indices["g92c5f29e1d66.jar"], 300, keys)]
    if args.only == "kernels":
        jobs = [(f"kernels-{i}", indices["@test-kernels.jar"], 0, "") for i in range(2)]
    if args.only == "mixed":
        jobs = [(f"mixed-{i}", indices[args.mixed_jar], 0, "") for i in range(2)]
    for name, index, frames, keys in jobs:
        print("BENCH_CASE", name, flush=True)
        c.send(f"bench {index} {frames} {keys}")
        lines = c.until("RUN_END", 120)
        audio = c.until("AUDIO_DETAIL", 10)
        perf = next((line for line in lines if line.startswith("PERF ")), "")
        fields = dict(re.findall(r"(\w+)=([^ ]+)", perf))
        result = {"case": name, "perf": fields,
                  "jit": next((line for line in lines if line.startswith("JIT_STATS ")), ""),
                  "jit_memory": next((line for line in lines if line.startswith("JIT_MEMORY ")), ""),
                  "jit_detail": next((line for line in lines if line.startswith("JIT_DETAIL ")), ""),
                  "end": lines[-1], "audio": audio,
                  "vm": next((line for line in lines if line.startswith("stats:")), ""),
                  "checks": [line for line in lines if "PERF_BENCH" in line or "AUDIO_BENCH" in line or "RENDER_BENCH" in line or line.startswith(("KERNEL", "MIXED "))]}
        results.append(result)
        args.output.write_text(json.dumps(results, indent=2) + "\n")
        if "rc=0" not in lines[-1] or not perf or any("WHOLE SCRIPT IS IGNORED" in line for line in lines):
            raise RuntimeError(f"Benchmark failed: {name}: {lines[-1]}")
        if not any(f"RUN_BEGIN index={index} " in line and "res=240x320" in line for line in lines):
            raise RuntimeError("Unexpected game or resolution")
        if name.startswith("cpu") and not any("PERF_BENCH frames=120 work=4000 checksum=-1082513742" in line for line in lines):
            raise RuntimeError("CPU benchmark work/checksum changed")
        if name.startswith("render") and not any("RENDER_BENCH frames=240 final=240" in line for line in lines):
            raise RuntimeError("Render benchmark did not finish all redraws")
        if fields.get("hot_live", "0") != "0" or fields.get("scale_mismatch", "0") != "0":
            raise RuntimeError(f"Memory or scaler invariant failed: {name}: {fields}")
        if not re.search(r"delta=0\b", lines[-1]):
            raise RuntimeError(f"PSRAM not recovered: {lines[-1]}")
        if not keys and not re.search(r"input=0\b", lines[-1]):
            raise RuntimeError(f"Unexpected input invalidated the benchmark: {lines[-1]}")
        if any(line.startswith("FPS_STATE on") for line in lines):
            raise RuntimeError("FPS overlay was enabled during the benchmark")
        if result["jit_memory"] and not re.search(r"live=0\b", result["jit_memory"]):
            raise RuntimeError("Executable memory was not recovered")
        if args.jit != "keep" and not any(f'JIT_BEGIN requested={int(args.jit != "off")} active={int(args.jit != "off")}' in line for line in lines):
            raise RuntimeError("Requested JIT mode was not active")
        if args.jit == "probe" and not re.search(r"compiled=0 executed=0 bytecodes=0", result["jit"]):
            raise RuntimeError("Probe-only mode executed generated code")
        if args.jit == "on" and name.startswith("cpu") and not re.search(r"executed=[1-9]\d*", result["jit"]):
            raise RuntimeError("CPU benchmark did not execute generated machine code")
        if name.startswith("audio"):
            for kind in ("WAV", "MIDI"):
                duration=next(re.search(r"duration=(\d+)", line)[1] for line in lines if f"AUDIO_BENCH_BEGIN {kind}" in line)
                if not any(f"AUDIO_BENCH_END {kind} time={duration} state=300" in line for line in lines):
                    raise RuntimeError(f"{kind} playback did not finish completely")
        if name.startswith("kernels"):
            expected={"locals":"2f66c58","constants":"839a908","fields":"4c1935ff","arrays":"452ca3","branches":"a632cf73"}
            actual={m[1]:m[2] for line in lines if (m:=re.match(r"KERNEL name=(\w+) iterations=50000 checksum=([0-9a-f]+) elapsed_ms=\d+$",line))}
            if actual != expected or "KERNELS_DONE" not in lines:
                raise RuntimeError(f"Pure kernel checksum mismatch: {actual}")
        if name.startswith("mixed") and not any(re.fullmatch(r"MIXED iterations=50000 checksum=553f49c9 elapsed_ms=\d+", line) for line in lines):
            raise RuntimeError("Mixed kernel checksum mismatch")
        time.sleep(0.15)
finally:
    c.serial.close()
