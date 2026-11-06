#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes a synthetic game folder for the zstd pack oracle.

    make_fixture.py OUT_DIR [--big]

Files around every rule of the writer: compressible and incompressible data,
exactly 256 KiB and 256 KiB + 1, files at the old 1% store-only line, an empty
file, configs, media and root files (packed since ps5-dump-forge v0.5.3),
executables and system folders in other letter cases (never packed), and
(--big) files of tens of MiB so compression runs over many batches.
"""
import json
import os
import random
import sys

CHUNK = 1 << 18


def text(size, seed):
    r = random.Random(seed)
    words = [b"mesh ", b"level ", b"texture ", b"0.25 ", b"{\n", b"}\n", b"alpha ", b"shader "]
    out = bytearray()
    while len(out) < size:
        out += r.choice(words)
    return bytes(out[:size])


def noise(size, seed):
    return random.Random(seed).randbytes(size)


def mixed(size, seed):
    # Half text, half noise per 4 KiB: zstd saves a little, chunk by chunk.
    r = random.Random(seed)
    out = bytearray()
    while len(out) < size:
        out += text(4096, r.randrange(1 << 30)) if r.random() < 0.5 else noise(4096, r.randrange(1 << 30))
    return bytes(out[:size])


def edge(zeros, seed):
    last = bytearray(noise(CHUNK, seed + 1))
    last[:zeros] = bytes(zeros)
    return noise(3 * CHUNK, seed) + bytes(last)


def main():
    out = sys.argv[1]
    big = "--big" in sys.argv
    files = {
        "eboot.bin": noise(70000, 1) + b"libSceAmpr.sprx\0" + noise(1000, 2),
        "sce_sys/param.json": json.dumps({"titleId": "ABCD99999", "contentVersion": "01.000.000"}).encode(),
        "sce_module/libc.prx": noise(3000, 3),
        "data/text.bin": text(6 * CHUNK + 77, 4),
        "data/noise.bin": noise(3 * CHUNK + 5, 5),
        "data/exact.bin": text(CHUNK, 6),
        "data/plus1.bin": text(CHUNK + 1, 7),
        "data/small.bin": text(1000, 8),
        "data/one.bin": b"x",
        "data/empty.bin": b"",
        "data/mixed.bin": mixed(5 * CHUNK + 999, 9),
        "data/sub/deep/edge-store.bin": edge(4 << 10, 10),
        "data/sub/deep/edge-packed.bin": edge(32 << 10, 12),
        "data/Upper/CaseName.BIN": text(70000, 14),
        "data/conf.json": text(2000, 15),
        "data/movie.mp4": noise(9000, 16),
        "data/system/inner.bin": text(5000, 17),
        "save/x.bin": text(2000, 18),
        "decrypted/x.bin": text(2000, 19),
        "data/decrypted/y.bin": text(20000, 20),
        "root.bin": text(3000, 21),
        "lib/module.sprx": noise(4000, 22),
        "data/EBOOT.BIN": noise(3000, 23),
        "Fakelib2/extra.bin": text(3000, 24),
        "Sce_Module/other.bin": text(3000, 25),
    }
    if big:
        files["data/big/text.bin"] = text(40 << 20, 30)
        files["data/big/mixed.bin"] = mixed(24 << 20, 31)
        files["data/big/noise.bin"] = noise(12 << 20, 32)
    for rel, data in files.items():
        path = os.path.join(out, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)


if __name__ == "__main__":
    main()
