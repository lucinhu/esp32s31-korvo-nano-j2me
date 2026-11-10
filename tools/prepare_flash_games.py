#!/usr/bin/env python3
"""Copy user-provided JARs into the local FAT image input; originals stay intact."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("directory", type=Path)
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
dest = root / "flash_data/games"
dest.mkdir(parents=True, exist_ok=True)
files = sorted(p for p in args.directory.iterdir() if p.is_file() and p.suffix.lower() == ".jar")
if not files:
    raise SystemExit("No JARs in input directory")
if sum(p.stat().st_size for p in files) > 7 * 1024 * 1024:
    raise SystemExit("JARs exceed 7 MiB; select fewer games to leave room for FAT and saves")
manifest = []
for file in files:
    digest = hashlib.sha256(file.read_bytes()).hexdigest()
    # IDF's FAT image generator cannot create Unicode 8.3 aliases correctly.
    # Use stable ASCII disk names, retain full display names in titles.tsv.
    stored_as = "g" + digest[:12] + ".jar"
    target = dest / stored_as
    if target.exists() and target.read_bytes() != file.read_bytes():
        raise SystemExit(f"Refusing to overwrite a different local game: {target}")
    shutil.copy2(file, target)
    manifest.append({"name": file.name, "stored_as": stored_as,
                     "bytes": file.stat().st_size, "sha256": digest})
    previous_copy = dest / file.name
    if previous_copy != target and previous_copy.exists() and previous_copy.read_bytes() == file.read_bytes():
        previous_copy.unlink()
(dest / "titles.tsv").write_text("".join(f"{m['stored_as']}\t{m['name']}\n" for m in manifest))
(root / "flash_data/games-manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
print(f"Prepared {len(files)} JARs, {sum(p.stat().st_size for p in files)} bytes")
