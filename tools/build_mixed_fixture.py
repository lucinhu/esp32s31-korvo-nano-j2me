#!/usr/bin/env python3
"""Wrap the upstream mixed kernel and cross-check its desktop JVM checksum."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile
import zipfile

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--output", type=Path, default=root / "build/performance/fixtures/perf-mixed.jar")
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory() as temp:
    temp = Path(temp)
    cp = subprocess.check_output(["python3", str(root / "third_party/nano_j2me/tools/midp_stubs.py"),
                                  "--out", str(temp / "stubs"), "--quiet"], text=True).strip()
    classes = temp / "classes"
    subprocess.run(["javac", "--release", "8", "-g:none", "-nowarn", "-classpath", cp,
                    "-d", str(classes), str(root / "tests/midlet/MixedBench.java"),
                    str(root / "third_party/nano_j2me/tests/jitperf/JitPerf.java")], check=True)
    oracle = subprocess.check_output(["java", "-cp", str(classes), "jitperf.JitPerf", "50000"], text=True)
    values = [i * 7 + 1 for i in range(32)]
    box = total = 0
    for i in range(50000):
        index = i & 31
        box = (box + i) & 0xffffffff
        values[index] = (values[index] + box) & 0xffffffff
        total ^= (values[index] + box) & 0xffffffff
        box ^= (i << 2) & 0xffffffff
    expected = f"{(total ^ box ^ values[50000 & 31]) & 0xffffffff:x}"
    if f"mixed={expected}" not in oracle.splitlines():
        raise RuntimeError(f"Independent checksum mismatch: expected {expected}, got {oracle}")
    with zipfile.ZipFile(args.output, "w", zipfile.ZIP_DEFLATED) as jar:
        jar.writestr("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\r\nMIDlet-Name: MixedBench\r\nMIDlet-1: MixedBench, , MixedBench\r\nMicroEdition-Profile: MIDP-2.0\r\nMicroEdition-Configuration: CLDC-1.1\r\n\r\n")
        for path in sorted(classes.rglob("*.class")):
            jar.write(path, str(path.relative_to(classes)))
    args.output.with_suffix(".oracle.txt").write_text(oracle)
print(f"{args.output} checksum={expected} sha256={hashlib.sha256(args.output.read_bytes()).hexdigest()}")
