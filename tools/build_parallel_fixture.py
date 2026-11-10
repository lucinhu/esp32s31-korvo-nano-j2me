#!/usr/bin/env python3
"""Build fixed-work rendering fixtures; no game assets required."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", type=Path)
parser.add_argument("--kind", choices=["m3g", "fill", "fill565", "form"], default="m3g")
args = parser.parse_args()
entry = {"m3g": "ParallelRenderBench", "fill": "FillBench", "fill565": "CanvasFillBench", "form": "FormScrollDemo"}[args.kind]
if args.output is None:
    args.output = root / "build/dualcore" / {"m3g": "perf-parallel.jar", "fill": "perf-simd.jar", "fill565": "perf-fill565.jar", "form": "ui-form-scroll.jar"}[args.kind]
args.output.parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    cp = subprocess.check_output(["python3", str(root / "third_party/nano_j2me/tools/midp_stubs.py"),
                                  "--out", str(temp / "stubs"), "--quiet"], text=True).strip()
    if args.kind == "m3g":
        m3g = subprocess.check_output(["python3", str(root / "third_party/nano_j2me/tools/m3g_stubs.py"),
                                      "--quiet"], text=True).strip()
        shutil.move(m3g, temp / "m3g")
        cp += os.pathsep + str(temp / "m3g")
    classes = temp / "classes"
    subprocess.run(["javac", "--release", "8", "-g:none", "-nowarn", "-classpath", cp,
                    "-d", str(classes), str(root / "tests/midlet" / (entry + ".java"))], check=True)
    with zipfile.ZipFile(args.output, "w", zipfile.ZIP_DEFLATED) as jar:
        jar.writestr("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\r\nMIDlet-Name: Parallel Render Bench\r\n"
                     f"MIDlet-1: Parallel Render Bench, , {entry}\r\n"
                     "MicroEdition-Profile: MIDP-2.0\r\nMicroEdition-Configuration: CLDC-1.1\r\n\r\n")
        for path in sorted(classes.glob("*.class")):
            jar.write(path, path.name)
print(args.output)
