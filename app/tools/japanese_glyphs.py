#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Writes the characters the app's Japanese face is baked with (UTF-8 text, for
# bake_font's file: glyph set): Japanese punctuation, kana, full-width and
# half-width forms, the 2,965 kanji of JIS X 0208 level 1 (everyday text,
# most game titles) and anything else the page's Japanese texts use.
#
#   app/tools/japanese_glyphs.py src/engine/web/i18n.ja.json > list.txt
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
    (0x3041, 0x3096),  # hiragana
    (0x3099, 0x30FF),  # kana marks, katakana
    (0x31F0, 0x31FF),  # small katakana
    (0xFF01, 0xFF5E),  # full-width forms
    (0xFF61, 0xFF9F),  # half-width katakana
]:
    for cp in range(lo, hi + 1):
        add(chr(cp))
# JIS X 0208 level 1 kanji: rows 16 to 47 (EUC-JP lead bytes 0xB0 to 0xCF).
for lead in range(0xB0, 0xD0):
    for trail in range(0xA1, 0xFF):
        try:
            add(bytes([lead, trail]).decode("euc_jp"))
        except UnicodeDecodeError:
            pass  # row 47 ends early
for path in sys.argv[1:]:
    with open(path, encoding="utf-8") as f:
        for text in json.load(f).values():
            for c in text:
                add(c)
sys.stdout.write("".join(chars) + "\n")
