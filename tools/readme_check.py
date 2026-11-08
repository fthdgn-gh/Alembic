#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Checks that every translated README (README.<code>.md) still matches
# README.md where a translation must not differ: the same code blocks, the
# same images, the same links to other files and the same number of headings,
# a language bar linking every README, and #anchors that exist in that file.
# It cannot tell whether the prose is up to date: after changing README.md,
# change the translations too, then run this.
#
#   python3 tools/readme_check.py
import os
import re
import sys

repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANGS = ["tr", "de", "es", "fr", "hu", "ru", "ja", "zh"]


def read(name):
    with open(os.path.join(repo, name), encoding="utf-8") as f:
        return f.read()


def parts(text):
    blocks = re.findall(r"^```.*?^```", text, re.S | re.M)
    prose = re.sub(r"^```.*?^```", "", text, flags=re.S | re.M)
    prose = re.sub(r"`[^`\n]*`", "", prose)
    headings = re.findall(r"^#{1,6} (.+)$", prose, re.M)
    targets = re.findall(r"\]\(([^)\s]+)", prose)
    images = [t for t in re.findall(r"!\[[^\]]*\]\(([^)\s]+)", prose)]
    files = sorted(t for t in targets if not t.startswith("#") and not re.fullmatch(r"README(\.\w+)?\.md", t))
    anchors = [t[1:] for t in targets if t.startswith("#")]
    return blocks, headings, images, files, anchors


def slug(heading):
    s = re.sub(r"[^\w\- ]", "", heading.strip().lower().replace("`", ""))
    return s.replace(" ", "-")


def main():
    base = parts(read("README.md"))
    problems = []
    for code in LANGS:
        name = f"README.{code}.md"
        if not os.path.exists(os.path.join(repo, name)):
            problems.append(f"{name}: missing")
            continue
        text = read(name)
        blocks, headings, images, files, anchors = parts(text)
        if blocks != base[0]:
            problems.append(f"{name}: code blocks differ from README.md")
        if len(headings) != len(base[1]):
            problems.append(f"{name}: {len(headings)} headings, README.md has {len(base[1])}")
        if images != base[2]:
            problems.append(f"{name}: images differ from README.md")
        if files != base[3]:
            problems.append(f"{name}: links to other files differ: {sorted(set(files) ^ set(base[3]))}")
        known = {slug(h) for h in headings}
        for a in anchors:
            if a not in known:
                problems.append(f"{name}: link to #{a}, which no heading makes")
        bar = text.split("\n")[2] if text.count("\n") > 2 else ""
        for other in ["README.md"] + [f"README.{c}.md" for c in LANGS if c != code]:
            if f"]({other})" not in bar:
                problems.append(f"{name}: the language bar (line 3) does not link {other}")
    for p in problems:
        print(p)
    print(f"{len(LANGS)} translations, {len(problems)} problems")
    return 1 if problems else 0


sys.exit(main())
