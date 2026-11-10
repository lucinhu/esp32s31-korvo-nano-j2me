#!/usr/bin/env python3
"""Prepare read-only application JARs without changing games or the submodule."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
import zipfile

root = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("rom_dir", type=Path)
parser.add_argument("--output", type=Path, default=root / "build/test-games")
args = parser.parse_args()
args.output = args.output.resolve()
args.output.mkdir(parents=True, exist_ok=True)
manifest = []
for alias, filename in [("dmc", "鬼泣3D中文版_3376.jar"), ("gof", "浴火银河_10189.jar"), ("moto", "3D摩托赛车_9287.jar")]:
    source = (args.rom_dir / filename).resolve(strict=True)
    target = args.output / (alias + ".jar")
    if target.is_symlink():
        if target.resolve() != source:
            raise ValueError(f"Existing alias points elsewhere: {target}")
    elif target.exists():
        raise FileExistsError(target)
    else:
        target.symlink_to(source)
    manifest.append({"alias": alias, "source": str(source), "size": source.stat().st_size,
                     "sha256": hashlib.sha256(source.read_bytes()).hexdigest()})
with tempfile.TemporaryDirectory() as work:
    work = Path(work)
    cp = subprocess.check_output(["python3", str(root / "third_party/nano_j2me/tools/midp_stubs.py"),
                                  "--out", str(work / "stubs"), "--quiet"], text=True).strip()
    classes = work / "classes"
    subprocess.run(["javac", "--release", "8", "-g:none", "-nowarn", "-classpath", cp, "-d", str(classes),
                    str(root / "tests/midlet/KernelBench.java"),
                    str(root / "third_party/nano_j2me/tests/jitperf/JitPerf.java")], check=True)
    with zipfile.ZipFile(args.output / "kernels.jar", "w", zipfile.ZIP_DEFLATED) as jar:
        jar.writestr("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\r\nMIDlet-Name: KernelBench\r\nMIDlet-1: KernelBench, , KernelBench\r\nMicroEdition-Profile: MIDP-2.0\r\nMicroEdition-Configuration: CLDC-1.1\r\n\r\n")
        for path in sorted(classes.rglob("*.class")):
            jar.write(path, str(path.relative_to(classes)))
    oracle = subprocess.check_output(["java", "-cp", str(classes), "jitperf.JitPerf", "50000"], text=True)
    (args.output / "kernel-oracle.txt").write_text(oracle)
(args.output / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
print(args.output)
