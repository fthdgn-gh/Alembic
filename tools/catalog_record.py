#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Writes the console app's record for darkxex/ps5-homebrew-catalog
# (apps/PPSA99594.json, its docs/submitting.md and docs/metadata.md) from a
# published release: the app's folder ZIP, GitHub's own SHA-256 of it, and the
# icon pinned to the tag.
#
#   tools/catalog_record.py v0.3.0 [--author NAME] > PPSA99594.json
#
# Needs the GitHub CLI (gh), logged in or not. The catalog checks the record
# against GitHub, so nothing here is guessed: a missing asset or digest stops it.
import argparse, json, subprocess, sys, unicodedata
from pathlib import Path

REPO = "fthdgn-gh/Alembic"
TITLE_ID = "PPSA99594"
DESCRIPTION = "Converts PS5 and PS4 packages and game dumps between FPKG, exFAT, FFPKG, FFPFS and FFPFSC images, on the console or with a PC."

def gh(*args):
    return json.loads(subprocess.run(["gh", "api", *args], check=True, capture_output=True, text=True).stdout)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tag")
    ap.add_argument("--author", default=subprocess.run(["git", "config", "user.name"], capture_output=True, text=True).stdout.strip())
    a = ap.parse_args()
    repo = gh(f"repos/{REPO}")
    release = gh(f"repos/{REPO}/releases/tags/{a.tag}")
    if release.get("draft"):
        sys.exit(f"{a.tag} is a draft; the catalog takes published releases only")
    zips = [x for x in release["assets"] if x["name"].startswith("alembic-app-") and x["name"].endswith(".zip")]
    if len(zips) != 1:
        sys.exit(f"{a.tag} has {len(zips)} app ZIPs (alembic-app-*.zip); it needs exactly one")
    asset = zips[0]
    digest = asset.get("digest") or ""
    if not digest.startswith("sha256:"):
        sys.exit(f"GitHub reports no SHA-256 for {asset['name']}")
    # The committed param.json at the tag: its contentVersion is what consoles compare.
    param = json.loads(subprocess.run(["git", "show", f"{a.tag}:app/sce_sys/param.json"], check=True, capture_output=True, text=True).stdout)
    if param.get("titleId") != TITLE_ID:
        sys.exit(f"param.json at {a.tag} has titleId {param.get('titleId')}")
    record = {
        "titleid": TITLE_ID,
        "name": "Alembic",
        "kind": "app",
        "category": "utilities",
        "description": DESCRIPTION,
        "license": repo["license"]["spdx_id"],  # what GitHub detects; the catalog checks that
        "author": unicodedata.normalize("NFC", a.author),
        "version": a.tag[1:] if a.tag.startswith("v") else a.tag,
        "source_repo": repo["html_url"],
        "artifact_url": asset["browser_download_url"],
        "sha256": digest.split(":", 1)[1].lower(),
        "icon_url": f"https://raw.githubusercontent.com/{repo['full_name']}/{a.tag}/app/sce_sys/icon0.png",
    }
    for k, v in record.items():
        if not v or v != v.strip():
            sys.exit(f"{k} is empty or has surrounding spaces")
    if len(record["description"]) > 200 or len(record["author"]) > 64:
        sys.exit("description (200) or author (64) is too long")
    text = json.dumps(record, indent=2, ensure_ascii=False) + "\n"
    if len(text.encode()) > 8192:
        sys.exit("the record is over 8 KiB")
    sys.stdout.write(text)
    print(f"contentVersion at {a.tag}: {param.get('contentVersion')}", file=sys.stderr)

if __name__ == "__main__":
    main()
