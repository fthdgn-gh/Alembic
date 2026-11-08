#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Writes the characters the app's Simplified Chinese face is baked with
# (UTF-8 text, for bake_font's file: glyph set): CJK punctuation, full-width
# forms, the 3,755 hanzi of GB 2312 level 1 (everyday text, most game titles)
# and anything else the page's Chinese texts use. Kana stay with the Japanese
# face.
#
#   app/tools/chinese_glyphs.py src/engine/web/i18n.zh.json > list.txt
import json
import sys

chars = []
seen = set()


def add(c):
    if c not in seen and ord(c) >= 0x80:
        seen.add(c)
        chars.append(c)


for lo, hi in [
    (0x3000, 0x303F),  # CJK symbols and punctuation
    (0xFF01, 0xFF5E),  # full-width forms
]:
    for cp in range(lo, hi + 1):
        add(chr(cp))
# GB 2312 level 1 hanzi: rows 16 to 55 (EUC-CN lead bytes 0xB0 to 0xD7).
for lead in range(0xB0, 0xD8):
    for trail in range(0xA1, 0xFF):
        try:
            add(bytes([lead, trail]).decode("gb2312"))
        except UnicodeDecodeError:
            pass  # row 55 ends early
for path in sys.argv[1:]:
    with open(path, encoding="utf-8") as f:
        for text in json.load(f).values():
            for c in text:
                add(c)
sys.stdout.write("".join(chars) + "\n")
