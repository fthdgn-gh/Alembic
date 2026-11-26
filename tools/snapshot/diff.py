#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compares two engine snapshots (POST /api/snapshot, src/engine/snapshot.hpp).

    tools/snapshot/diff.py BEFORE AFTER [--all-mtimes]

BEFORE and AFTER are snapshot folders copied off the console (listing.tsv,
files/, summary.json). Prints the entries added, removed and changed
(size, SHA-256, kind; modification times only with --all-mtimes, since the
system touches many files on its own), then, for every SQLite database
copied in both, the rows added and removed per table, and the tables and
columns that appeared.
"""
import argparse
import os
import shutil
import sqlite3
import sys
import tempfile


def unescape(s):
    out, i = [], 0
    while i < len(s):
        if s[i] == "%" and i + 3 <= len(s):
            out.append(chr(int(s[i + 1:i + 3], 16)))
            i += 3
        else:
            out.append(s[i])
            i += 1
    return "".join(out)


def load(folder):
    entries, errors = {}, {}
    with open(os.path.join(folder, "listing.tsv"), encoding="utf-8", errors="surrogateescape") as f:
        for line in f:
            if line.startswith("#"):
                continue
            parts = line.rstrip("\n").split("\t")
            if parts[0] == "error":
                errors[unescape(parts[4])] = unescape(parts[5]) if len(parts) > 5 else ""
                continue
            kind, size, mtime, digest, path = parts[:5]
            extra = (parts + ["", "", "", ""])[5:9]  # mode, uid, gid, link target
            entries[unescape(path)] = (kind, int(size), int(mtime), digest, extra[0], extra[1], extra[2], unescape(extra[3]))
    return entries, errors


def copies(folder):
    root = os.path.join(folder, "files")
    found = {}
    for dirpath, _, names in os.walk(root):
        for n in names:
            full = os.path.join(dirpath, n)
            found["/" + os.path.relpath(full, root)] = full
    return found


def is_sqlite(path):
    try:
        with open(path, "rb") as f:
            return f.read(16) == b"SQLite format 3\x00"
    except OSError:
        return False


def tables(path):
    # Opened from a temporary copy with its -wal/-journal beside it: the
    # console's database was live, and rows may still be in its WAL (opening
    # it immutable, or without the WAL, would miss them).
    tmp = tempfile.mkdtemp(prefix="alembic-snapdb-")
    base = os.path.join(tmp, "db")
    shutil.copyfile(path, base)
    siblings = []
    for suffix in ("-wal", "-journal"):
        if os.path.exists(path + suffix):
            shutil.copyfile(path + suffix, base + suffix)
            siblings.append(suffix)
    if siblings:
        print("  (%s had %s beside it, read with it)" % (os.path.basename(path), ", ".join(siblings)))
    con = sqlite3.connect(base)
    try:
        out = {}
        names = [r[0] for r in con.execute("SELECT name FROM sqlite_master WHERE type='table'")]
        for t in names:
            cur = con.execute('SELECT * FROM "%s"' % t.replace('"', '""'))
            cols = [d[0] for d in cur.description]
            out[t] = (cols, [tuple(r) for r in cur.fetchall()])
        return out
    finally:
        con.close()
        shutil.rmtree(tmp, ignore_errors=True)


def short(v, n=120):
    s = repr(v)
    return s if len(s) <= n else s[:n] + "..."


def diff_db(name, a, b):
    ta, tb = tables(a), tables(b)
    printed = False

    def head():
        nonlocal printed
        if not printed:
            print("\n== database %s" % name)
            printed = True

    for t in sorted(set(ta) | set(tb)):
        if t not in ta:
            head()
            print("  table added: %s (%d rows) columns %s" % (t, len(tb[t][1]), tb[t][0]))
            continue
        if t not in tb:
            head()
            print("  table removed: %s" % t)
            continue
        ca, ra = ta[t]
        cb, rb = tb[t]
        if ca != cb:
            head()
            print("  %s: columns %s -> %s" % (t, ca, cb))
            continue
        sa, sb = set(ra), set(rb)
        added = [r for r in rb if r not in sa]
        removed = [r for r in ra if r not in sb]
        if not added and not removed:
            continue
        head()
        print("  %s: +%d -%d rows" % (t, len(added), len(removed)))
        for label, rows in (("-", removed), ("+", added)):
            for r in rows:
                print("    %s {%s}" % (label, ", ".join("%s=%s" % (c, short(v)) for c, v in zip(ca, r) if v not in (None, ""))))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("--all-mtimes", action="store_true", help="also list entries whose only change is the time")
    args = ap.parse_args()

    a, ea = load(args.before)
    b, eb = load(args.after)
    added = sorted(set(b) - set(a))
    removed = sorted(set(a) - set(b))
    changed, touched = [], []
    for p in sorted(set(a) & set(b)):
        ka, sa, ma, da = a[p][:4]
        kb, sb, mb, db = b[p][:4]
        if ka != kb or (ka == "file" and (sa != sb or da != db)) or a[p][4:] != b[p][4:]:
            changed.append(p)
        elif ma != mb:
            touched.append(p)

    def fmt(e):
        kind, size, mtime, digest, mode, uid, gid, target = e
        s = "%-5s %12d %-16s %s %s:%s" % (kind, size, digest[:16] if digest != "-" else "-", mode or "-", uid or "-", gid or "-")
        return s + (" -> " + target if target else "")

    print("== added (%d)" % len(added))
    for p in added:
        print("  + %s  %s" % (fmt(b[p]), p))
    print("\n== removed (%d)" % len(removed))
    for p in removed:
        print("  - %s  %s" % (fmt(a[p]), p))
    print("\n== changed (%d)" % len(changed))
    for p in changed:
        print("  ~ %s -> %s  %s" % (fmt(a[p]), fmt(b[p]), p))
    print("\n== time only (%d)%s" % (len(touched), "" if args.all_mtimes else ", --all-mtimes lists them"))
    if args.all_mtimes:
        for p in touched:
            print("  t %s" % p)
    new_errors = sorted(set(eb) - set(ea))
    if new_errors:
        print("\n== unreadable only in AFTER (%d)" % len(new_errors))
        for p in new_errors:
            print("  ! %s: %s" % (p, eb[p]))

    ca, cb = copies(args.before), copies(args.after)
    for p in sorted(set(ca) & set(cb)):
        if is_sqlite(ca[p]) and is_sqlite(cb[p]):
            try:
                diff_db(p, ca[p], cb[p])
            except sqlite3.DatabaseError as e:
                print("\n== database %s: cannot read (%s)" % (p, e))
    for p in sorted(set(cb) - set(ca)):
        if is_sqlite(cb[p]):
            print("\n== database only in AFTER: %s" % p)
    return 0


if __name__ == "__main__":
    sys.exit(main())
