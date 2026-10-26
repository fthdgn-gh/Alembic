#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes a synthetic PS4 game folder and the GP4 project LibOrbisPkg's
pkg_build reads for it, listing every folder and file in the order Alembic's
PS4 package writer uses, which is the order pkg_makegp4 gives a project:
sce_sys first; in each folder, sub-folders holding CNT entry files first (by
lowest entry id), then by name; entry files first (by id), then by name.

    make_game.py OUT CASE      -> OUT/game/... and OUT/Project.gp4
CASE: small (keystone generated), media (keystone, changeinfo, more entries),
large (a file needing doubly indirect blocks in the outer PFS, ~130 MB)."""
import os, struct, sys
from xml.sax.saxutils import quoteattr

CONTENT_ID = "UP0000-CUSA00000_00-0000000000000000"
STAMP = "2020-01-02 03:04:05"


def pattern(size, seed):
    return bytes((seed + i * 7 + (i >> 10)) & 0xFF for i in range(size))


GENERATED = {"license.dat", "license.info", "psreserved.dat", "playgo-chunk.dat", "playgo-chunk.sha", "playgo-manifest.xml"}


def entry_names():
    """LibOrbisPkg's EntryNames (PKG/Enums.cs), name -> id."""
    names = {
        "nptitle.dat": 0x402, "npbind.dat": 0x403, "selfinfo.dat": 0x404, "imageinfo.dat": 0x406,
        "target-deltainfo.dat": 0x407, "origin-deltainfo.dat": 0x408, "param.sfo": 0x1000,
        "pronunciation.xml": 0x1004, "pronunciation.sig": 0x1005, "pic1.png": 0x1006, "pubtoolinfo.dat": 0x1007,
        "app/playgo-chunk.dat": 0x1008, "app/playgo-chunk.sha": 0x1009, "app/playgo-manifest.xml": 0x100A,
        "shareparam.json": 0x100B, "shareoverlayimage.png": 0x100C, "save_data.png": 0x100D,
        "shareprivacyguardimage.png": 0x100E, "icon0.png": 0x1200, "pic0.png": 0x1220, "snd0.at9": 0x1240,
        "changeinfo/changeinfo.xml": 0x1260, "icon0.dds": 0x1280, "pic0.dds": 0x12A0, "pic1.dds": 0x12C0,
    }
    for i in range(31):
        names["icon0_%02d.png" % i] = 0x1201 + i
        names["icon0_%02d.dds" % i] = 0x1281 + i
        names["pic1_%02d.png" % i] = 0x1241 + i
        names["pic1_%02d.dds" % i] = 0x12C1 + i
        names["changeinfo/changeinfo_%02d.xml" % i] = 0x1261 + i
        if i < 10:
            names["keymap_rp/0%02d.png" % (i + 1)] = 0x1600 + i
        for j in range(10):
            names["keymap_rp/%02d/0%02d.png" % (i, j + 1)] = 0x1610 + 16 * i + j
    for i in range(100):
        names["trophy/trophy%02d.trp" % i] = 0x1400 + i
    return names


ENTRIES = entry_names()


def entry_of(sys_rel):
    """The entry id of a path inside sce_sys (None outside it or for generated ones)."""
    if sys_rel is None or sys_rel in GENERATED:
        return None
    return ENTRIES.get(sys_rel)


def sfo(values):
    """A PSF file; values: (key, str|int, max length)."""
    rows, keys, data = b"", b"", b""
    for key, value, max_length in values:
        if isinstance(value, int):
            raw, fmt, length, max_length = struct.pack("<I", value), 0x0404, 4, 4
        else:
            raw = value.encode() + b"\0"
            fmt, length = 0x0204, len(raw)
        rows += struct.pack("<HHIII", len(keys), fmt, length, max_length, len(data))
        keys += key.encode() + b"\0"
        data += raw.ljust(max_length, b"\0")
    while (0x14 + len(rows) + len(keys)) % 4:
        keys += b"\0"
    header = b"\0PSF" + struct.pack("<IIII", 0x101, 0x14 + len(rows), 0x14 + len(rows) + len(keys), len(values))
    return header + rows + keys + data


def main():
    out, case = sys.argv[1], sys.argv[2]
    game = os.path.join(out, "game")
    files = {
        "eboot.bin": pattern(200000, 1),
        "data/big.bin": pattern(13 * 65536 + 7, 2),
        "data/sub/empty.bin": b"",
        "data/sub/Small_File.txt": pattern(10, 3),
        "data/x-y.bin": pattern(11, 4),
        "data/x_y.bin": pattern(12, 5),
        "data/x.y.bin": pattern(13, 6),
        "data/xY2.bin": pattern(14, 7),
        "0_": pattern(4, 8),  # "/0_" and "/1@" share a path-table hash
        "1@": pattern(5, 9),
        "sce_sys/icon0.png": pattern(5000, 10),
        "sce_sys/nptitle.dat": pattern(160, 11),
        "sce_sys/trophy/trophy00.trp": pattern(3000, 12),
        "sce_sys/param.sfo": sfo([
            ("APP_TYPE", 1, 4), ("APP_VER", "01.00", 8), ("ATTRIBUTE", 0, 4), ("ATTRIBUTE2", 0, 4),
            ("CATEGORY", "gd", 4), ("CONTENT_ID", CONTENT_ID, 48), ("FORMAT", "obs", 4),
            ("PUBTOOLINFO", "c_date=20200102,c_time=030405", 0x200), ("TITLE", "Synthetic", 128),
            ("TITLE_ID", "CUSA00000", 12), ("VERSION", "01.00", 8)]),
    }
    if case in ("media", "large"):
        files.update({
            "sce_sys/keystone": pattern(96, 13),
            "sce_sys/pic1.png": pattern(70000, 14),
            "sce_sys/pic0.png": pattern(9000, 15),
            "sce_sys/icon0.dds": pattern(131200, 16),
            "sce_sys/npbind.dat": pattern(532, 17),
            "sce_sys/pronunciation.xml": pattern(2055, 18),
            "sce_sys/pronunciation.sig": pattern(20, 19),
            "sce_sys/changeinfo/changeinfo.xml": pattern(145, 20),
            "sce_sys/about/right.sprx": pattern(16025, 21),
            "sce_module/libc.prx": pattern(9000, 22),
        })
    if case == "large":
        files["data/huge.bin"] = None  # written in pieces below
    for rel, data in files.items():
        path = os.path.join(game, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            if data is None:
                piece = pattern(1 << 20, 23)
                for _ in range(130):
                    f.write(piece)
            else:
                f.write(data)

    # The GP4: folders and files in Alembic's order.
    dirs, gp4_files = [], []

    def sys_rel(rel, name):
        path = (rel + "/" + name) if rel else name
        return path[len("sce_sys/"):] if path.startswith("sce_sys/") else None

    def lowest(rel):
        best = 1 << 40
        for root, _, names in os.walk(os.path.join(game, rel)):
            for n in names:
                r = os.path.relpath(os.path.join(root, n), game).replace(os.sep, "/")
                e = entry_of(r[len("sce_sys/"):] if r.startswith("sce_sys/") else None)
                if e is not None:
                    best = min(best, e)
        return best

    def walk(rel, depth):
        here = os.path.join(game, rel)
        names = os.listdir(here)
        subdirs = [n for n in names if os.path.isdir(os.path.join(here, n))]
        files = [n for n in names if n not in subdirs]
        child = lambda n: (rel + "/" + n) if rel else n
        subdirs.sort(key=lambda n: (not (rel == "" and n == "sce_sys"),
                                    lowest(child(n)) if child(n).startswith("sce_sys/") else 1 << 40, n.encode()))
        files.sort(key=lambda n: (entry_of(sys_rel(rel, n)) if entry_of(sys_rel(rel, n)) is not None else 1 << 40,
                                  n.encode()))
        for d in subdirs:
            dirs.append("  " * depth + "    <dir targ_name=%s>" % quoteattr(d))
            walk(child(d), depth + 1)
            dirs.append("  " * depth + "    </dir>")
        for n in files:
            gp4_files.append('    <file targ_path=%s orig_path=%s />' % (quoteattr(child(n)), quoteattr("game/" + child(n))))

    walk("", 0)
    gp4 = "\n".join([
        '<?xml version="1.0" encoding="utf-8" standalone="yes"?>',
        '<psproject fmt="gp4" version="1000">',
        "  <volume>",
        "    <volume_type>pkg_ps4_app</volume_type>",
        "    <volume_id>PS4VOLUME</volume_id>",
        "    <volume_ts>%s</volume_ts>" % STAMP,
        '    <package content_id="%s" passcode="%s" storage_type="digital50" app_type="full" c_date="%s" />'
        % (CONTENT_ID, "0" * 32, STAMP),
        "  </volume>",
        '  <files img_no="0">',
        *gp4_files,
        "  </files>",
        "  <rootdir>",
        *dirs,
        "  </rootdir>",
        "</psproject>",
        "",
    ])
    with open(os.path.join(out, "Project.gp4"), "w") as f:
        f.write(gp4)


main()
