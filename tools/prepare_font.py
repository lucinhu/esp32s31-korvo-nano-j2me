#!/usr/bin/env python3
"""Build the bundled simplified/traditional Chinese font (SIL OFL 1.1).

Only regeneration needs fontTools. Normal firmware builds use the saved OTF.
"""
import argparse
from pathlib import Path

from fontTools import subset
from fontTools.ttLib import TTFont


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--face", type=int, default=2, help="SC face in the Noto CJK TTC")
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1] / "assets/fonts/j2me_font.otf")
    args = parser.parse_args()
    points = set(range(0x20, 0x100)) | {0xFFFD}
    for lead in range(0xA1, 0xF8):
        for trail in range(0xA1, 0xFF):
            try:
                points.add(ord(bytes((lead, trail)).decode("gb2312")))
            except UnicodeDecodeError:
                pass
    # Big5 symbols and level-one (common) Hanzi. GB2312 alone omits even
    # basic Traditional Chinese UI words such as 開關 and 確認. Stop at
    # C67E: level-two rare Hanzi would exceed the 8 MiB application partition.
    for lead in range(0xA1, 0xC7):
        for trail in range(0x40, 0xFF):
            if lead == 0xC6 and trail > 0x7E:
                continue
            try:
                points.add(ord(bytes((lead, trail)).decode("big5")))
            except UnicodeDecodeError:
                pass
    font = TTFont(args.source, fontNumber=args.face)
    opts = subset.Options()
    opts.name_IDs = [0, 1, 2, 3, 4, 5, 6, 13, 14]
    opts.name_legacy = True
    opts.name_languages = [0x409]
    # The renderer performs character lookup directly; it needs no shaping
    # substitutions or the extra glyphs reachable only through those tables.
    opts.layout_features = []
    opts.layout_closure = False
    # Retain outlines/metrics, omit CFF hints to fit both character sets in
    # Flash. FreeType still rasterises the outlines with grayscale coverage.
    opts.hinting = False
    worker = subset.Subsetter(options=opts)
    worker.populate(unicodes=points)
    worker.subset(font)
    names = {1: "Korvo Sans", 2: "Regular", 3: "KorvoSans-Regular", 4: "Korvo Sans Regular", 6: "KorvoSans-Regular"}
    for record in font["name"].names:
        if record.nameID in names:
            record.string = names[record.nameID].encode(record.getEncoding())
    if "CFF " in font:
        font["CFF "].cff.fontNames = ["KorvoSans-Regular"]
        top = font["CFF "].cff.topDictIndex[0]
        top.FullName = "Korvo Sans Regular"
        top.FamilyName = "Korvo Sans"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    font.save(args.output)
    print(f"{args.output}: {args.output.stat().st_size} bytes, {len(font.getBestCmap())} code points")


if __name__ == "__main__":
    main()
