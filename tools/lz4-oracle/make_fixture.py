#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes a synthetic game folder that exercises the LZ4 packer: compressible
and random data, block-boundary sizes, duplicate blocks, a large random file
(auto-loose) and a large compressible one, empty files, hot audio folders,
root numbered files, safety-listed types, odd glob characters and non-ASCII
names. `--layout unity` / `--layout insomniac` write a game with that
engine's file layout instead, for the bundled profiles. Deterministic (fixed
seed and mtimes)."""
import os
import random
import sys

root = sys.argv[1]
big = "--big" in sys.argv[2:]
layout = sys.argv[sys.argv.index("--layout") + 1] if "--layout" in sys.argv else ""
rng = random.Random(1234)
MTIME = 1700000000


def text(n):
    words = [b"alpha", b"beta", b"gamma", b"delta", b"mesh", b"texture", b"level", b"\n", b"{", b"}", b"0.5"]
    out = bytearray()
    while len(out) < n:
        out += rng.choice(words) + b" "
    return bytes(out[:n])


def noise(n):
    return rng.randbytes(n)


def mixed(n):
    out = bytearray()
    while len(out) < n:
        out += text(rng.randint(100, 9000)) if rng.random() < 0.6 else noise(rng.randint(100, 5000))
    return bytes(out[:n])


def put(rel, data, mtime=MTIME):
    path = os.path.join(root, *rel.split("/"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    os.utime(path, (mtime, mtime))


K = 1024


def common():
    put("eboot.bin", noise(300 * K))
    put("sce_sys/param.json", b'{"titleId": "TEST00002"}')
    put("sce_module/libc.prx", noise(20 * K))
    put("fakelib/libSceAmpr.sprx", noise(50 * K))


if layout == "unity":  # a PS5 Unity build: Unity's data files in Media/
    common()
    put("Media/globalgamemanagers", mixed(90 * K))
    put("Media/globalgamemanagers.assets", mixed(120 * K))
    put("Media/level0", mixed(70 * K))
    put("Media/level0.resS", text(200 * K))
    put("Media/sharedassets0.assets", mixed(150 * K))
    put("Media/boot.config", text(1 * K))
    put("Media/Metadata/global-metadata.dat", text(300 * K))
    put("Media/Plugins/libfmod.prx", noise(40 * K))
    put("Media/StreamingAssets/aa/catalog.json", text(20 * K))
    put("Media/StreamingAssets/aa/PS5/level_assets_all.bundle", mixed(400 * K))
    put("Media/StreamingAssets/aa/PS5/shared_assets_all.bundle", text(260 * K))
    put("Media/StreamingAssets/PS5/Master.bank", mixed(180 * K))
    put("Media/StreamingAssets/SaveIcon.png", noise(30 * K))
    sys.exit(0)
if layout == "insomniac":  # toc, dag and d/ archives at the top
    common()
    put("toc", text(10 * K))
    put("dag", text(12 * K))
    put("commandline.txt", text(1 * K))
    put("d/a/archive_000", mixed(300 * K))
    put("d/a/archive_001", text(200 * K))
    put("d/a/bootload_000", mixed(100 * K))
    put("d/b/movie_000", noise(120 * K))
    put("d/b/wem_000", mixed(90 * K))
    put("d/b/soundbank_000", mixed(80 * K))
    sys.exit(0)

put("eboot.bin", noise(300 * K))
put("sce_sys/param.json", b'{"titleId": "TEST00001", "contentId": "UP0000-TEST00001_00-0000000000000000"}')
put("sce_sys/icon0.png", noise(40 * K))
put("sce_module/libc.prx", noise(20 * K))
put("fakelib/libSceAmpr.sprx", noise(50 * K))
put("data.0", text(200 * K))
put("chunk1.bin", mixed(130 * K))
put("readme.txt", text(5 * K))
put("empty_root.dat", b"")
for i, size in enumerate([1, 63, 64, 65, 16 * K - 1, 64 * K - 1, 64 * K, 64 * K + 1, 128 * K, 300 * K + 7]):
    put(f"assets/text_{i:02d}.res", text(size), MTIME + i)
    put(f"assets/noise_{i:02d}.res", noise(size), MTIME + i)
    put(f"assets/mixed_{i:02d}.uasset", mixed(size), MTIME + i)
put("assets/empty.res", b"")
# Identical blocks inside and across files (de-duplication).
block = text(64 * K)
put("assets/dup_a.bin", block * 5 + noise(1000))
put("assets/dup_b.bin", noise(64 * K) + block * 3)
put("assets/textures/rock.dds", mixed(200 * K))
put("assets/textures/sky.gnf", mixed(90 * K))
put("sound/sfx/hit.bank", mixed(120 * K))
put("sound/vo/line.bank", text(70 * K))
put("audio/music.soundbank", mixed(50 * K))
put("movies/intro.bk2", noise(100 * K))
put("movies/credits.bin", noise(100 * K))
put("cache_ps5/playgo.pgm", text(4 * K))
put("data/[weird] name.bin", mixed(70 * K))
put("data/été/naïve.res", text(33 * K))
put("data/deep/a/b/c/d.res", mixed(250 * K))
put("data/config.json", text(3 * K))
if big:
    put("bulk/random.pak2", noise(70 * 1024 * K))
    put("bulk/text.pak2", text(70 * 1024 * K))
