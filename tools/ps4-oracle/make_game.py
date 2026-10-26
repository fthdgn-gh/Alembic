#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes a synthetic PS4 game folder and the GP4 project LibOrbisPkg's
pkg_build reads for it, listing every folder and file in the order Alembic's
PS4 package writer uses (each folder's sub-folders, then its files, by name).

    make_game.py OUT CASE      -> OUT/game/... and OUT/Project.gp4
CASE: small (keystone generated), media (keystone, changeinfo, more entries),
large (a file needing doubly indirect blocks in the outer PFS, ~130 MB)."""
import os, struct, sys
from xml.sax.saxutils import quoteattr

CONTENT_ID = "UP0000-CUSA00000_00-0000000000000000"
STAMP = "2020-01-02 03:04:05"


def pattern(size, seed):
    return bytes((seed + i * 7 + (i >> 10)) & 0xFF for i in range(size))


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

    def walk(rel, depth):
        here = os.path.join(game, rel)
        names = sorted(os.listdir(here), key=lambda n: n.encode())
        subdirs = [n for n in names if os.path.isdir(os.path.join(here, n))]
        for d in subdirs:
            dirs.append("  " * depth + "    <dir targ_name=%s>" % quoteattr(d))
            walk(os.path.join(rel, d) if rel else d, depth + 1)
            dirs.append("  " * depth + "    </dir>")
        for n in names:
            if n not in subdirs:
                target = (rel + "/" + n) if rel else n
                gp4_files.append('    <file targ_path=%s orig_path=%s />' % (quoteattr(target), quoteattr("game/" + target)))

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
