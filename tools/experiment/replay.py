#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the replay and undo manifests for POST /api/experiment
(src/engine/experiment.hpp) from two engine snapshots of one system install
(tools/snapshot/diff.py BEFORE AFTER shows the same changes).

    tools/experiment/replay.py BEFORE AFTER --title ID --package PKG
        --files-from PREFIX --package-from PATH [--root PREFIX] [--out DIR]

The replay puts back, without the system installer, what the install wrote:
its folders and files (app.pkg and app.si cut from the package, the rest
from AFTER's copies) and the title's rows in app.db and appinfo.db, exactly
as AFTER holds them. Before that it backs up both databases on the console.
The undo removes those rows, files and folders again; it can run again, or
after a replay that stopped part way (missing rows and files are fine).

  PKG           the package on this computer (checked against app.pkg, app.si)
  PACKAGE_FROM  the same package as the console engine names it
                ("/peer/<this engine's id>/<path>", or a path on the console)
  PREFIX        AFTER's files/ folder as the console engine names it
  --root        put every target path under this folder (tests on a PC)

Left out: the background-transfer task (/user/bgft/task/..., with bgft.db),
the notification, system temp files, the snapshots themselves.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import sqlite3
import sys
import tempfile
import time

SKIP = ("/user/bgft/", "/user/temp/", "/user/data/alembic-snapshots", "/user/data/alembic/")
MMS = "/system_data/priv/mms/"


def unescape(s):
    return re.sub(r"%([0-9A-F]{2})", lambda m: chr(int(m.group(1), 16)), s)


def load(folder):
    out = {}
    with open(os.path.join(folder, "listing.tsv"), encoding="utf-8", errors="surrogateescape") as f:
        for line in f:
            if line.startswith("#"):
                continue
            p = line.rstrip("\n").split("\t")
            if p[0] == "error":
                continue
            out[unescape(p[4])] = {"kind": p[0], "size": int(p[1]), "sha256": p[3], "mode": p[5] if len(p) > 5 else ""}
    return out


def open_copy(path):
    tmp = tempfile.mkdtemp(prefix="alembic-replay-")
    base = os.path.join(tmp, "db")
    shutil.copyfile(path, base)
    for s in ("-wal", "-journal"):
        if os.path.exists(path + s):
            shutil.copyfile(path + s, base + s)
    return sqlite3.connect(base)


def param(v):
    if isinstance(v, bytes):
        return {"blob_hex": v.hex()}
    return v


def title_rows(snapshot, title):
    """Every row of the title in app.db and appinfo.db: by its title ID, or
    by its concept ID (local_concept_id). {db: {table: [(columns, row)]}}"""
    found = {}
    for name in ("app.db", "appinfo.db"):
        con = open_copy(os.path.join(snapshot, "files" + MMS + name))
        if name == "app.db":
            cids = {r[0] for r in con.execute("SELECT localConceptId FROM tbl_contentinfo WHERE titleId=?", (title,))}
        else:
            cids = {r[0] for r in con.execute("SELECT val FROM tbl_appinfo WHERE titleId=? AND key='_local_concept_id'", (title,))}
        tables = [r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type='table'")]
        for t in tables:
            cols = [d[1] for d in con.execute('PRAGMA table_info("%s")' % t)]
            for r in con.execute('SELECT * FROM "%s"' % t):
                if any(v == title for v in r) or any(isinstance(v, str) and v in cids for v in r):
                    found.setdefault(name, {}).setdefault(t, []).append((cols, r))
        versions = {r[0]: r[1] for r in con.execute("SELECT category, status FROM tbl_version")}
        found.setdefault(name, {})["__version__"] = versions
        found[name]["__cids__"] = sorted(cids)
        con.close()
    return found


def sha256_file(path, offset=0, length=None):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        f.seek(offset)
        left = length if length is not None else os.path.getsize(path) - offset
        while left:
            b = f.read(min(left, 4 << 20))
            if not b:
                raise SystemExit("%s is shorter than expected" % path)
            h.update(b)
            left -= len(b)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("--title", required=True)
    ap.add_argument("--package", required=True)
    ap.add_argument("--package-from", required=True)
    ap.add_argument("--files-from", required=True)
    ap.add_argument("--root", default="")
    ap.add_argument("--backup", default="/data/alembic-replay/backup")
    ap.add_argument("--out", default=".")
    args = ap.parse_args()
    T = args.title
    R = args.root.rstrip("/")

    before, after = load(args.before), load(args.after)
    added = sorted(p for p in set(after) - set(before) if not p.startswith(SKIP))
    if not added:
        raise SystemExit("AFTER adds nothing to BEFORE")

    # app.pkg and app.si: the package cut at the SI, as the installer did.
    pkg_size = os.path.getsize(args.package)
    app_pkg = "/user/app/%s/app.pkg" % T
    si = [p for p in added if p.endswith("/app.si") and p.startswith("/user/playgo/app/")]
    if app_pkg not in after or len(si) != 1:
        raise SystemExit("AFTER has no app.pkg or app.si for %s" % T)
    cut = after[app_pkg]["size"]
    if cut + after[si[0]]["size"] != pkg_size:
        raise SystemExit("app.pkg + app.si is not the package's size")
    for path, off, length in ((app_pkg, 0, cut), (si[0], cut, pkg_size - cut)):
        if sha256_file(args.package, off, length) != after[path]["sha256"]:
            raise SystemExit("%s does not match the package's bytes" % path)

    ops, undo = [], []
    stamp = time.strftime("%Y%m%d-%H%M%S")
    for name in ("app.db", "appinfo.db"):
        ops.append({"op": "backup_db", "db": R + MMS + name, "to": "%s%s-%s/%s" % (R, args.backup, stamp, name)})

    dirs = [p for p in added if after[p]["kind"] == "dir"]
    files = [p for p in added if after[p]["kind"] == "file"]
    others = [p for p in added if after[p]["kind"] not in ("dir", "file")]
    if others:
        raise SystemExit("not handled (links?): %s" % others)
    for d in sorted(dirs, key=lambda p: p.count("/")):
        ops.append({"op": "mkdir", "path": R + d, "mode": after[d]["mode"]})
    for f in files:
        e = after[f]
        op = {"op": "copy", "path": R + f, "mode": e["mode"]}
        if f == app_pkg:
            op.update({"from": args.package_from, "offset": 0, "length": cut})
        elif f == si[0]:
            op.update({"from": args.package_from, "offset": cut, "length": pkg_size - cut})
        else:
            local = os.path.join(args.after, "files" + f)
            if not os.path.exists(local):
                raise SystemExit("AFTER has no copy of %s" % f)
            if e["sha256"] != "-" and sha256_file(local) != e["sha256"]:
                raise SystemExit("AFTER's copy of %s does not match its listing" % f)
            op.update({"from": args.files_from.rstrip("/") + f, "length": e["size"]})
        ops.append(op)

    rows_before = title_rows(args.before, T)
    rows_after = title_rows(args.after, T)
    for name in ("app.db", "appinfo.db"):
        tables = {t: v for t, v in rows_before[name].items() if not t.startswith("__")}
        if tables:
            raise SystemExit("BEFORE already has rows of %s in %s: %s" % (T, name, sorted(tables)))
        statements, removes = [], []
        for t, rows in sorted(rows_after[name].items()):
            if t.startswith("__"):
                continue
            for cols, r in rows:
                statements.append({
                    "sql": 'INSERT INTO "%s" (%s) VALUES (%s)' % (t, ", ".join('"%s"' % c for c in cols), ", ".join("?" * len(cols))),
                    "params": [param(v) for v in r], "changes": 1})
            # The undo: by the column that holds the title or its concept.
            cols = rows[0][0]
            key = next((c for c in cols if c.lower() in ("titleid",)), None)
            if key:
                removes.append({"sql": 'DELETE FROM "%s" WHERE "%s"=?' % (t, key), "params": [T]})
            else:
                ckey = next(c for c in cols if c.lower() in ("localconceptid", "local_concept_id"))
                for cid in rows_after[name]["__cids__"]:
                    removes.append({"sql": 'DELETE FROM "%s" WHERE "%s"=?' % (t, ckey), "params": [cid]})
        # The counters as they were after the install, never lowered.
        for cat, status in sorted(rows_after[name]["__version__"].items()):
            statements.append({"sql": "UPDATE tbl_version SET status=max(status, ?) WHERE category=?", "params": [status, cat]})
        ops.append({"op": "sql", "db": R + MMS + name, "statements": statements})
        undo.append({"op": "sql", "db": R + MMS + name, "statements": removes})

    for f in reversed(files):
        undo.append({"op": "remove", "path": R + f})
    for d in sorted(dirs, key=lambda p: -p.count("/")):
        undo.append({"op": "remove", "path": R + d})

    os.makedirs(args.out, exist_ok=True)
    for name, body in (("replay.json", {"ops": ops}), ("undo.json", {"ops": undo})):
        with open(os.path.join(args.out, name), "w") as f:
            json.dump(body, f, indent=1)
    n_rows = sum(len(o["statements"]) for o in ops if o["op"] == "sql")
    print("replay: %d folders, %d files, %d SQL statements; undo: %d ops" % (len(dirs), len(files), n_rows, len(undo)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
