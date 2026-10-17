#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Creates dump-like folders that exercise the exFAT/AMPR edge cases."""
import os, random, sys

def write(path, size, seed):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    rng = random.Random(seed)
    with open(path, "wb") as f:
        # half pattern (compressible), half noise
        half = size // 2
        f.write(bytes((i * 7 + seed) & 0xFF for i in range(half)))
        f.write(rng.randbytes(size - half))
    os.utime(path, (1700000000 + seed, 1700000000 + seed))

def small(root):
    write(f"{root}/eboot.bin", 300_000, 1)
    write(f"{root}/sce_sys/param.json", 1234, 2)
    write(f"{root}/sce_sys/icon0.png", 32768, 3)          # exactly one 32 KiB cluster
    write(f"{root}/sce_sys/about/right.sprx", 32769, 4)   # one byte over
    write(f"{root}/Data/empty.bin", 0, 5)
    os.makedirs(f"{root}/Data/Empty Folder", exist_ok=True)
    for i, name in enumerate(["B.dat", "a.dat", "_x.dat", "Z1.dat", "ä.dat", "École.dat",
                              "日本.dat", "\U0001F600.dat", "x" * 255]):
        write(f"{root}/Data/{name}", 100 + i, 10 + i)
    write(f"{root}/.DS_Store", 10, 30)                    # ignored
    write(f"{root}/Data/._a.dat", 10, 31)                 # ignored

def ampr(root):
    small(root)
    write(f"{root}/fakelib/libSceAmpr.sprx", 5000, 40)
    write(f"{root}/ampr_emu.index", 64, 41)               # stale, replaced

def amprclean(root):
    # For ampr_emu's own builder, which indexes every file it finds, so no
    # ignored names here. Non-ASCII names are the case that matters.
    for i, name in enumerate(["eboot.bin", "sce_sys/param.json", "Data/B.dat", "Data/a.dat",
                              "Data/_x.dat", "Data/\u00e4.dat", "Data/\u00c9cole.dat",
                              "Data/\u65e5\u672c.dat", "Data/\U0001F600.dat", "Data/" + "x" * 200,
                              "fakelib/libSceAmpr.sprx", "ampr_commands.bin"]):
        write(f"{root}/{name}", 50 + i * 13, 300 + i)

def text(path, content):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(content)
    os.utime(path, (1700000000, 1700000000))

def dumps(root):
    """Folders that exercise FFPFSC's game-root search and payload naming."""
    def game(path, param):
        text(f"{path}/sce_sys/param.json", param)
        write(f"{path}/eboot.bin", 70_000, 400)
    game(f"{root}/named", b'{"contentId": "x", "titleId": "ppsa01234", "n": [1, {"a": null}]}')
    game(f"{root}/bom", b'\xef\xbb\xbf{"titleId": "PPSA05555"}')
    game(f"{root}/case", b'{"TitleID": "CUSA00001", "titleId": "bad"}')
    game(f"{root}/notid", b'{"titleId": "PPSA1234"}')
    game(f"{root}/trailing", b'{"titleId": "PPSA01234",}')
    game(f"{root}/numberid", b'{"titleId": 12345, "TITLEID": "ABCD99999"}')
    game(f"{root}/nonobject", b'["titleId", "PPSA01234"]')
    game(f"{root}/escaped", b'{"titl\\u0065Id": "PPS\\u004101234"}')
    game(f"{root}/ Jeu \u00e9t\u00e9: \U0001F600. ", b'{}')
    game(f"{root}/wrapped/a/b/PPSA07777-app", b'{"titleId": "PPSA07777"}')
    os.makedirs(f"{root}/wrapped/z_empty", exist_ok=True)

def many(root):
    for i in range(1500):                                  # root dir spans several clusters
        write(f"{root}/files/file_{i:05d}_with_a_longer_name.bin", i % 700, 100 + i)
    deep = root
    for d in range(12):
        deep += f"/level{d}"
        write(f"{deep}/f.bin", 10 + d, 2000 + d)

def large(root):                                          # average >= 1 MiB -> 64 KiB clusters
    write(f"{root}/eboot.bin", 3 * 1024 * 1024 + 5, 50)
    write(f"{root}/sce_sys/param.json", 500, 51)

def empty(root):
    os.makedirs(root, exist_ok=True)

if __name__ == "__main__":
    out = sys.argv[1]
    for fn in (small, ampr, amprclean, many, large, empty):
        fn(os.path.join(out, fn.__name__))
    # Dump folders for FFPFSC go next to the fixtures: "wrapped" must not sit
    # under a folder that other game roots share.
    dumps(os.path.join(os.path.dirname(os.path.abspath(out)), "dumps"))
