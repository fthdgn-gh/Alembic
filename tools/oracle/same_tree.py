#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compares two folders file by file, ignoring OS junk files (as Alembic does)
and, with --skip, named files at the root. Exit 0 when they match."""
import argparse, filecmp, os, sys

JUNK = {".ds_store", ".spotlight-v100", ".trashes", ".fseventsd", ".temporaryitems",
        ".documentrevisions-v100", ".apdisk", "__macosx", ".volumeicon.icns",
        "thumbs.db", "ehthumbs.db", "desktop.ini", "$recycle.bin", "system volume information"}

def listing(root, skip):
    out = {}
    for dirpath, dirnames, filenames in os.walk(root):
        rel = os.path.relpath(dirpath, root)
        dirnames[:] = [d for d in dirnames if d.lower() not in JUNK and not d.startswith("._")]
        for d in dirnames:
            out[os.path.normpath(os.path.join(rel, d))] = None
        for f in filenames:
            if f.lower() in JUNK or f.startswith("._"):
                continue
            path = os.path.normpath(os.path.join(rel, f))
            if path in skip:
                continue
            out[path] = os.path.join(dirpath, f)
    return out

ap = argparse.ArgumentParser()
ap.add_argument("a"); ap.add_argument("b"); ap.add_argument("--skip", action="append", default=[])
args = ap.parse_args()
la, lb = listing(args.a, set(args.skip)), listing(args.b, set(args.skip))
if la.keys() != lb.keys():
    print("different entries:", sorted(la.keys() ^ lb.keys())[:5], file=sys.stderr)
    sys.exit(1)
for path, full in la.items():
    if full is not None and not filecmp.cmp(full, lb[path], shallow=False):
        print("different contents:", path, file=sys.stderr)
        sys.exit(1)
