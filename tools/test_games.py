#!/usr/bin/env python3
"""Sample uploaded games with fresh RMS; record counters without claiming playability."""
import argparse
from collections import Counter
import json
from pathlib import Path
import re
from serial_console import Console

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path)
parser.add_argument("--scenarios", type=Path, default=Path(__file__).resolve().parent.parent / "tests/game_scenarios.json")
parser.add_argument("--port", default="/dev/ttyUSB0")
parser.add_argument("--jit", choices=["on", "off", "keep"], default="keep")
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
cases = json.loads(args.scenarios.read_text())
c = Console(args.port, 115200)
results = []
try:
    c.drain(1)
    c.send("stats"); status = c.until("STATUS ", 10)[-1]; c.until("AUDIO_DETAIL", 10)
    if "running=1" in status:
        c.send("stop"); c.until("RUN_END", 30); c.until("AUDIO_DETAIL", 10)
    c.send("fps off"); c.until("FPS_STATE off", 10)
    if args.jit != "keep":
        c.send(f"jit {args.jit}"); c.until(f"JIT_STATE {args.jit}", 10)
    c.send("volume 40"); c.until("VOLUME 40", 10)
    c.send("list"); listing = c.until("GAMES_END", 10)
    indices = {}
    for line in listing:
        m = re.match(r"(\d+) (\S+\.jar)(?: |$)", line)
        if m: indices[Path(m[2]).name] = int(m[1])
    for case in cases:
        print("GAME_CASE", case["file"], flush=True)
        c.send(f'res {case["width"]} {case["height"]}'); c.until("RES_OK", 10)
        c.send(f'bench {indices[case["file"]]} {case["frames"]} {case.get("keys", "")}')
        lines = c.until("RUN_END", 180)
        audio = c.until("AUDIO_DETAIL", 10)
        perf = next((line for line in lines if line.startswith("PERF ")), "")
        counters = dict(re.findall(r"(\w+)=([^ ]+)", perf))
        result = {"scenario": case, "end": lines[-1], "perf": counters, "audio": audio,
                  "jit": next((line for line in lines if line.startswith("JIT_STATS ")), ""),
                  "jit_memory": next((line for line in lines if line.startswith("JIT_MEMORY ")), ""),
                  "jit_detail": next((line for line in lines if line.startswith("JIT_DETAIL ")), ""),
                  "vm": next((line for line in lines if line.startswith("stats:")), ""),
                  "exceptions": dict(Counter(line for line in lines if line.startswith("thrown:")))}
        results.append(result)
        args.output.write_text(json.dumps(results, ensure_ascii=False, indent=2)+"\n")
        if counters.get("hot_live", "0") != "0" or not re.search(r"delta=0\b", lines[-1]):
            raise RuntimeError(f'Memory was not recovered: {case["file"]}')
        if "rc=0" not in lines[-1] or not perf or counters.get("scale_mismatch") != "0":
            raise RuntimeError(f'Runtime or scaler failure: {case["file"]}')
        if result["jit_memory"] and not re.search(r"live=0\b", result["jit_memory"]):
            raise RuntimeError("Executable memory was not recovered")
        if args.jit != "keep" and not any(f'JIT_BEGIN requested={int(args.jit == "on")} active={int(args.jit == "on")}' in line for line in lines):
            raise RuntimeError("Requested JIT mode was not active")
        if any("WHOLE SCRIPT IS IGNORED" in line for line in lines):
            raise RuntimeError("Key script rejected")
    c.send("res 240 320"); c.until("RES_OK", 10)
    c.send("storage"); c.until("STORAGE", 10)
    print("GAME_SAMPLES_COMPLETE", len(results), flush=True)
finally:
    c.serial.close()
