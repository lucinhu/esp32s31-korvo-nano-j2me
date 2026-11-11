#!/usr/bin/env python3
"""Measure existing FPS telemetry after navigation, with fresh benchmark RMS."""
import argparse
import contextlib
import io
import json
from pathlib import Path
import re
import statistics
import time
from serial_console import Console, screenshot as console_screenshot

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path)
parser.add_argument("--game", required=True, help="JAR basename or embedded alias")
parser.add_argument("--attach", action="store_true", help="Sample the already running game after manual navigation; keep its RMS and execution settings")
parser.add_argument("--jit", choices=["on", "off"], required=True)
parser.add_argument("--jit-memory", choices=["keep", "internal", "mapped"], default="keep",
                    help="Select executable storage when the firmware supports it")
parser.add_argument("--keys", default="", help="Comma-separated navigation keys or wait:SECONDS")
movement = parser.add_mutually_exclusive_group()
movement.add_argument("--hold", default="", help="Key held through warmup and sampling")
movement.add_argument("--repeat-keys", default="", help="Cycle comma-separated key pulses through warmup and sampling")
parser.add_argument("--repeat-period", type=float, default=.5, help="Seconds per repeated key pulse (half down, half up)")
parser.add_argument("--startup-wait", type=float, default=4)
parser.add_argument("--key-hold", type=float, default=.4, help="Duration of each navigation key press")
parser.add_argument("--skip-before-shot", action="store_true", help="Avoid a capture stall before timing titles that automatically pause")
parser.add_argument("--settle", type=float, default=0, help="Wait after navigation for level loading")
parser.add_argument("--samples", type=int, default=20)
parser.add_argument("--sample-interval", type=float, default=1, help="Seconds between FPS queries")
parser.add_argument("--warmup", type=float, default=5)
parser.add_argument("--counter-fps", action="store_true", help="Measure updated frame deltas with the FPS overlay disabled (requires updated STATUS telemetry)")
parser.add_argument("--game-shots", action="store_true", help="Capture the raw game image before/after sampling")
parser.add_argument("--strict-shots", action="store_true", help="Reject a run if screenshot transfer needed a retry")
parser.add_argument("--port", default="/dev/ttyUSB0")
args = parser.parse_args()
if args.samples < 1 or args.sample_interval <= 0 or args.repeat_period <= 0 or args.key_hold < 0 or args.startup_wait < 0 or args.warmup < 0 or args.settle < 0:
    parser.error("Sample count must be positive and delays nonnegative")
args.output.parent.mkdir(parents=True, exist_ok=True)
c = Console(args.port,115200)
result = {"game":args.game,"jit":args.jit,"resolution":[240,320],
          "jit_memory_mode":args.jit_memory,
          "warmup_seconds":args.warmup,"sample_interval_seconds":args.sample_interval,
          "keys":args.keys,"held_key":args.hold,"samples":[],
          "repeated_keys":args.repeat_keys,"repeat_period_seconds":args.repeat_period,
          "screenshot_source":"game" if args.game_shots else "lcd",
          "no_screenshots_during_sampling":True}
result["measurement"] = "updated counter / wall time" if args.counter_fps else "FPS overlay telemetry"
result["attached_to_running_game"] = args.attach
result["skip_before_screenshot"] = args.skip_before_shot
result["navigation_key_hold_seconds"] = args.key_hold
repeat_keys = list(filter(None, args.repeat_keys.split(",")))
repeating = False
repeat_down = False
repeat_index = 0
repeat_next = 0
repeat_count = 0

def screenshot(*shot_args):
    output = io.StringIO()
    started = time.monotonic()
    try:
        with contextlib.redirect_stdout(output):
            console_screenshot(*shot_args)
    finally:
        print(output.getvalue(), end="", flush=True)
    retried = "Fast screenshot failed" in output.getvalue()
    result.setdefault("screenshots", []).append({
        "seconds": time.monotonic() - started, "retried": retried})
    if args.strict_shots and retried:
        raise RuntimeError("SCENE_INVALID: screenshot retry changed the scene timing")

def wait(seconds):
    global repeat_down, repeat_index, repeat_next, repeat_count
    # Keep startup progress visible even while large resources are loading.
    deadline = time.monotonic()+seconds
    while time.monotonic()<deadline:
        now = time.monotonic()
        if repeating and now >= repeat_next:
            key(repeat_keys[repeat_index], "up" if repeat_down else "down")
            if repeat_down:
                repeat_index = (repeat_index + 1) % len(repeat_keys)
            else:
                repeat_count += 1
            repeat_down = not repeat_down
            repeat_next = time.monotonic() + args.repeat_period / 2
        pause = min(5, deadline-time.monotonic())
        if repeating:
            pause = min(pause, max(0, repeat_next-time.monotonic()))
        if pause > 0:
            c.drain(pause)

def key(name,edge):
    c.send(f"key {name} {edge}"); c.until("KEY_OK",60)

try:
    c.drain(.3)
    c.send("stats");state=c.until("STATUS ",10)[-1];c.until("AUDIO_DETAIL",10)
    if args.attach:
        if "running=1" not in state:
            raise RuntimeError("--attach requires a running game")
        c.send("settings");settings=c.until("SETTINGS",10)[-1]
        if "res=240x320" not in settings or f"jit={args.jit} " not in settings:
            raise RuntimeError("Running game settings do not match the requested resolution/JIT")
        if args.jit_memory != "keep" and f"memory={args.jit_memory} " not in settings:
            raise RuntimeError("Running game code memory does not match --jit-memory")
    elif "running=1" in state:
        c.send("stop");c.until("RUN_END",120);c.until("AUDIO_DETAIL",10)
    if not args.attach and args.jit_memory != "keep":
        c.send(f"jit memory {args.jit_memory}");c.until(f"JIT_MEMORY_STATE {args.jit_memory}",10)
    commands=[("fps off","FPS_STATE off")]
    if not args.attach:
        commands += [(f"jit {args.jit}",f"JIT_STATE {args.jit}"),("res 240 320","RES_OK"),("volume 40","VOLUME 40")]
    for command,marker in commands:
        c.send(command);c.until(marker,10)
    c.send("list");listing=c.until("GAMES_END",10)
    entries=[(int(m[1]),Path(m[2]).name) for line in listing
             if (m:=re.match(r"(\d+) (\S+\.jar)(?: |$)",line))]
    index=next(index for index,name in entries if name==args.game)
    if not args.attach:
        c.send(f"bench {index} 0");c.until("RUN_BEGIN",10)
    wait(args.startup_wait)
    for name in filter(None,args.keys.split(",")):
        if name.startswith("wait:"):
            delay = float(name.split(":", 1)[1])
            if delay < 0:
                raise ValueError("Navigation waits must be nonnegative")
            wait(delay)
            continue
        key(name,"down");wait(args.key_hold);key(name,"up");wait(.6)
    wait(args.settle)
    if not args.skip_before_shot:
        screenshot(c,args.output.with_name(args.output.stem+"-before.png"),args.game_shots,2000000)
    if args.hold:key(args.hold,"down")
    repeating = bool(repeat_keys)
    if not args.counter_fps:
        c.send("fps on");c.until("FPS_STATE on",10)
    wait(args.warmup)
    c.send("stats");initial_status=c.until("STATUS ",10)[-1]
    previous_when=time.monotonic()
    if args.counter_fps:
        fields=dict(re.findall(r"(\w+)=(\d+)",initial_status))
        if "updated" not in fields:
            raise RuntimeError("Firmware does not expose updated STATUS telemetry")
        previous_updated=int(fields["updated"])
    result["sample_audio_before"]=c.until("AUDIO_DETAIL",10)
    start=time.monotonic()
    for i in range(args.samples):
        if args.counter_fps:
            wait(args.sample_interval)
        when=time.monotonic()
        sample={"sample":i,"time_seconds":when-start}
        if args.counter_fps:
            c.send("stats");line=c.until("STATUS ",10)[-1]
            now=time.monotonic()
            fields=dict(re.findall(r"(\w+)=(\d+)",line))
            if fields.get("running") != "1":
                raise RuntimeError("Game exited during sampling")
            updated=int(fields["updated"])
            delta=(updated-previous_updated) & 0xffffffff
            value=delta/(now-previous_when)
            sample.update(updated=updated,delta=delta,interval_seconds=now-previous_when)
            previous_updated,previous_when=updated,now
            c.until("AUDIO_DETAIL",10)
        else:
            c.send("fps");line=c.until("FPS_STATE on rate=",10)[-1]
            value=float(re.search(r"rate=([0-9.]+)",line)[1])
        sample["fps"]=value
        result["samples"].append(sample)
        if not args.counter_fps:
            wait(max(0,args.sample_interval-(time.monotonic()-when)))
    result["sampling_seconds"]=time.monotonic()-start
    repeating = False
    if repeat_down:
        key(repeat_keys[repeat_index], "up")
    result["repeated_pulses_including_warmup"] = repeat_count
    if args.hold:key(args.hold,"up")
    values=[s["fps"] for s in result["samples"]]
    result.update(mean=statistics.mean(values),median=statistics.median(values),
                  minimum=min(values),maximum=max(values))
    c.send("stats");result["active_status"]=c.until("STATUS ",10)[-1]
    result["sample_audio_after"]=c.until("AUDIO_DETAIL",10)
    for prefix,label in [("AUDIO_DETAIL","detail"),("AUDIO writes=","output")]:
        before=next(line for line in result["sample_audio_before"] if prefix in line)
        after=next(line for line in result["sample_audio_after"] if prefix in line)
        old=dict(re.findall(r"(\w+)=(\d+)",before))
        new=dict(re.findall(r"(\w+)=(\d+)",after))
        result["sample_audio_"+label+"_delta"]={name:int(new[name])-int(old[name]) for name in old}
    if "running=1" not in result["active_status"]:raise RuntimeError("Game exited during sampling")
    screenshot(c,args.output.with_name(args.output.stem+"-after.png"),args.game_shots,2000000)
    c.send("fps off");c.until("FPS_STATE off",10)
    c.send("stop");lines=c.until("RUN_END",120);result["audio"]=c.until("AUDIO_DETAIL",10)
    result["end"]=lines[-1]
    for prefix,name in [("stats:","vm"),("PERF ","perf"),("JIT_STATS ","jit_stats"),
                        ("JIT_DETAIL ","jit_detail"),("JIT_MEMORY ","jit_memory"),("JIT_POOL ","jit_pool"),
                        ("DISPLAY_IDLE ","display_idle")]:
        result[name]=next((line for line in lines if line.startswith(prefix)),"")
    args.output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+"\n")
    if "rc=0" not in result["end"] or "delta=0" not in result["end"]:
        raise RuntimeError("Game failed or PSRAM was not recovered")
    if "hot_live=0" not in result["perf"] or "scale_mismatch=0" not in result["perf"]:
        raise RuntimeError("Memory/scaler check failed")
    if "live=0" not in result["jit_memory"]:raise RuntimeError("Executable memory was not recovered")
    if args.jit_memory == "mapped" and args.jit == "on":
        if "released=1" not in result["jit_pool"] or "map_failures=0" not in result["jit_pool"]:
            raise RuntimeError("Executable mapping failed or was not released")
    if args.jit=="on" and not re.search(r"executed=[1-9]\d*",result["jit_stats"]):
        raise RuntimeError("No generated code executed")
    print(f"SCENE_FPS mean={result['mean']:.3f} median={result['median']:.3f} min={result['minimum']} max={result['maximum']}",flush=True)
finally:
    c.serial.close()
