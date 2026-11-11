# Korvo Sans

`j2me_font.otf` is a renamed subset of **Noto Sans CJK SC Regular**, containing
GB2312, Big5 symbols and level-one common Traditional Chinese (through C67E),
Latin-1 and the replacement character. It covers 9,833 Unicode code points,
including all Chinese string literals in PAL 2008. Source: the SC face (index 2) in
Noto Sans CJK Regular 2.004, distributed in Arch Linux's `noto-fonts-cjk` package.
The original copyright and license records are retained inside the font.

Upstream: https://github.com/notofonts/noto-cjk

License: SIL Open Font License 1.1, reproduced in [LICENSE](LICENSE).
The font remains under that license, independently of the application code.

The subset omits CFF hint instructions, retaining glyph outlines and metrics.
This keeps the font near 2.1 MiB so the firmware still fits the existing 8 MiB
application partition. Big5 level-two rare characters are outside this subset.

Regenerate with Python and fontTools:

```sh
python3 tools/prepare_font.py /path/to/NotoSansCJK-Regular.ttc
```

Firmware builds use the checked-in subset. To embed another TTF/OTF, configure
`-D KORVO_FONT_FILE=/absolute/path/to/font.ttf`. Keep its redistribution license
with your distribution. FreeType renders all six MIDP font slots; missing
characters retain the core's bitmap fallback.
