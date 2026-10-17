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
    for fn in (small, ampr, many, large, empty):
        fn(os.path.join(out, fn.__name__))
