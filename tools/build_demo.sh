#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
STUBS="$(python3 "$ROOT/third_party/nano_j2me/tools/midp_stubs.py" --out "$WORK/stubs" --quiet)"
javac --release 8 -nowarn -encoding UTF-8 -classpath "$STUBS" -d "$WORK/classes" "$ROOT/tests/midlet/KorvoDemo.java"
mkdir -p "$ROOT/assets"
python3 - "$WORK/classes" "$ROOT/assets/korvo_demo.jar" <<'PY'
import pathlib, sys, zipfile
root = pathlib.Path(sys.argv[1])
with zipfile.ZipFile(sys.argv[2], 'w', zipfile.ZIP_DEFLATED) as jar:
    entries = {'META-INF/MANIFEST.MF': b'Manifest-Version: 1.0\r\nMIDlet-Name: Korvo Demo\r\nMIDlet-Vendor: Korvo Port\r\nMIDlet-Version: 1.0\r\nMIDlet-1: Korvo Demo, , KorvoDemo\r\nMicroEdition-Profile: MIDP-2.0\r\nMicroEdition-Configuration: CLDC-1.1\r\n\r\n'}
    entries.update({p.name: p.read_bytes() for p in sorted(root.glob('*.class'))})
    for name, data in entries.items():
        info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        jar.writestr(info, data)
print('Generated', sys.argv[2])
PY
