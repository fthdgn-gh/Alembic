#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks a zstd-packed game folder against ampr-emu-ng's docs/zstd-format.md,
independently of Alembic's code, and against the original files.

    check_pack.py PACKED_DIR ORIGINAL_DIR [--level N]

Reads ampr_zstd.index and every ampr_zstd-NNN.pak by the documented layout
(both index CRCs, the volume header CRC, offsets, counts, order), decodes every
chunk with libzstd (ctypes; Homebrew's 1.5.7 on macOS) and compares it with
the original file. Then it re-encodes each chunk the way ps5-dump-forge's
writer.rs does (a context with the level, window log 18, checksum and content
size; the chunk pledged, fed with ZSTD_e_continue and closed with ZSTD_e_end
into a buffer of the chunk's size) and checks the codec choice and the frame
bytes; no file is store-only (ps5-dump-forge dropped its 1% rule in v0.5.3).
Exits non-zero on the first mismatch.
"""
import ctypes
import ctypes.util
import os
import struct
import sys
import zlib

CHUNK = 1 << 18
LIMIT = (1 << 32) - (1 << 16)


def load_zstd():
    names = [ctypes.util.find_library("zstd"), "libzstd.so.1", "libzstd.dylib", "/opt/homebrew/lib/libzstd.dylib"]
    for name in names:
        if not name:
            continue
        try:
            return ctypes.CDLL(name)
        except OSError:
            pass
    sys.exit("libzstd not found (brew install zstd)")


z = load_zstd()
z.ZSTD_versionNumber.restype = ctypes.c_uint
z.ZSTD_createCCtx.restype = ctypes.c_void_p
z.ZSTD_CCtx_setParameter.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
z.ZSTD_CCtx_setParameter.restype = ctypes.c_size_t
z.ZSTD_CCtx_reset.argtypes = [ctypes.c_void_p, ctypes.c_int]
z.ZSTD_CCtx_reset.restype = ctypes.c_size_t
z.ZSTD_CCtx_setPledgedSrcSize.argtypes = [ctypes.c_void_p, ctypes.c_ulonglong]
z.ZSTD_CCtx_setPledgedSrcSize.restype = ctypes.c_size_t
z.ZSTD_compressStream2.restype = ctypes.c_size_t
z.ZSTD_isError.argtypes = [ctypes.c_size_t]
z.ZSTD_isError.restype = ctypes.c_uint
z.ZSTD_decompress.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
z.ZSTD_decompress.restype = ctypes.c_size_t
z.ZSTD_getFrameContentSize.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
z.ZSTD_getFrameContentSize.restype = ctypes.c_ulonglong
z.ZSTD_findFrameCompressedSize.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
z.ZSTD_findFrameCompressedSize.restype = ctypes.c_size_t


class Buffer(ctypes.Structure):
    _fields_ = [("ptr", ctypes.c_void_p), ("size", ctypes.c_size_t), ("pos", ctypes.c_size_t)]


z.ZSTD_compressStream2.argtypes = [ctypes.c_void_p, ctypes.POINTER(Buffer), ctypes.POINTER(Buffer), ctypes.c_int]

contexts = {}


def encode(raw, level):
    """ps5-dump-forge's encode_chunk(): the frame, or None to store raw."""
    cctx = contexts.get(level)
    if cctx is None:
        cctx = z.ZSTD_createCCtx()
        for param, value in ((100, level), (101, 18), (201, 1), (200, 1)):
            assert not z.ZSTD_isError(z.ZSTD_CCtx_setParameter(cctx, param, value))
        contexts[level] = cctx
    assert not z.ZSTD_isError(z.ZSTD_CCtx_reset(cctx, 1))
    assert not z.ZSTD_isError(z.ZSTD_CCtx_setPledgedSrcSize(cctx, len(raw)))
    src = ctypes.create_string_buffer(raw, len(raw))
    dst = ctypes.create_string_buffer(len(raw))
    inb = Buffer(ctypes.cast(src, ctypes.c_void_p), len(raw), 0)
    out = Buffer(ctypes.cast(dst, ctypes.c_void_p), len(raw), 0)
    end = 0  # ZSTD_e_continue
    while True:
        left = z.ZSTD_compressStream2(cctx, ctypes.byref(out), ctypes.byref(inb), end)
        assert not z.ZSTD_isError(left)
        if end == 2 and left == 0:
            return dst.raw[: out.pos] if out.pos < len(raw) else None
        if out.pos == out.size:
            return None
        if inb.pos == len(raw):
            end = 2  # ZSTD_e_end


def fail(message):
    sys.exit("FAIL  " + message)


def fnv(path):
    h = 14695981039346656037
    for b in path:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    args = sys.argv[1:]
    level = None
    if "--level" in args:
        i = args.index("--level")
        level = int(args[i + 1])
        del args[i : i + 2]
    packed, original = args
    if z.ZSTD_versionNumber() != 10507:
        print(f"note  libzstd {z.ZSTD_versionNumber()} is not 1.5.7: frames may legitimately differ")
    data = open(os.path.join(packed, "ampr_zstd.index"), "rb").read()
    h = data[:128]
    if h[:8] != b"AMPRZPK1" or struct.unpack_from("<II", h, 8) != (1, 128) or h[16] != 18:
        fail("index magic, version, header size or chunk shift")
    if h[18:20] != b"\0\0" or h[104:112] != bytes(8) or h[120:128] != bytes(8):
        fail("index reserved bytes")
    if struct.unpack_from("<I", h, 112)[0] != zlib.crc32(data[128:]):
        fail("index payload CRC")
    if struct.unpack_from("<I", h, 116)[0] != zlib.crc32(h[:112] + bytes(8) + h[120:]):
        fail("index header CRC")
    lvl = h[17]
    if level is not None and lvl != level:
        fail(f"level {lvl}, expected {level}")
    (volumes,) = struct.unpack_from("<I", h, 20)
    archive_id = h[24:40]
    files, chunks, files_off, chunks_off, volumes_off, strings_off, strings_len, total = struct.unpack_from("<8Q", h, 40)
    if files_off != 128 or chunks_off != 128 + 32 * files or volumes_off != chunks_off + 8 * chunks:
        fail("table offsets")
    if strings_off != volumes_off + 8 * volumes or strings_off + strings_len != len(data):
        fail("strings offset or length")
    vol_sizes = [struct.unpack_from("<Q", data, volumes_off + 8 * i)[0] for i in range(volumes)]
    vol_bytes = []
    for i, size in enumerate(vol_sizes):
        name = f"ampr_zstd-{i:03d}.pak"
        v = open(os.path.join(packed, name), "rb").read()
        if len(v) != size or size > LIMIT:
            fail(f"{name} size")
        vh = v[:64]
        if vh[:8] != b"AMPRZDT1" or struct.unpack_from("<IIII", vh, 8) != (1, 64, i, volumes):
            fail(f"{name} header fields")
        if vh[24:40] != archive_id or struct.unpack_from("<Q", vh, 40)[0] != size - 64 or vh[48:60] != bytes(12):
            fail(f"{name} archive ID, payload size or reserved bytes")
        if struct.unpack_from("<I", vh, 60)[0] != zlib.crc32(vh[:60] + bytes(4)):
            fail(f"{name} header CRC")
        vol_bytes.append(v)
    expect_chunk = 0
    previous = None
    volume, cursor = 0, 64
    sum_sizes = 0
    stats = {"zstd": 0, "raw": 0}
    for i in range(files):
        r = 128 + 32 * i
        ph, size, first, path_off, path_len, flags, reserved = struct.unpack_from("<QQIIHHI", data, r)
        path = data[strings_off + path_off : strings_off + path_off + path_len]
        if ph != fnv(path) or flags & ~1 or reserved or first != expect_chunk:
            fail(f"file record {i}")
        if previous is not None and (ph, path) <= previous:
            fail("file order")
        previous = (ph, path)
        sum_sizes += size
        src = open(os.path.join(original, path.decode()), "rb").read()
        if len(src) != size:
            fail(f"{path!r} size")
        count = (size + CHUNK - 1) // CHUNK
        want_codecs = []
        for k in range(count):
            (word,) = struct.unpack_from("<Q", data, chunks_off + 8 * (first + k))
            offset, vid, stored, codec = word & 0xFFFFFFFF, (word >> 32) & 0xFFF, ((word >> 44) & 0x3FFFF) + 1, word >> 62
            raw = src[k * CHUNK : (k + 1) * CHUNK]
            if vid != volume:
                if vid != volume + 1 or vol_sizes[volume] != cursor:
                    fail("volume continuity")
                volume, cursor = vid, 64
            if offset != cursor:
                fail(f"{path!r} chunk {k} offset")
            cursor += stored
            body = vol_bytes[vid][offset : offset + stored]
            if codec == 0:
                if stored != len(raw) or body != raw:
                    fail(f"{path!r} raw chunk {k}")
                stats["raw"] += 1
            elif codec == 1:
                d = body[4] if len(body) > 4 else 0
                if body[:4] != b"\x28\xb5\x2f\xfd" or not d & 4 or d & 0x0B or not (d & 0xC0 or d & 0x20):
                    fail(f"{path!r} chunk {k} frame descriptor")
                if stored >= len(raw) or z.ZSTD_getFrameContentSize(body, stored) != len(raw):
                    fail(f"{path!r} chunk {k} frame size")
                if z.ZSTD_findFrameCompressedSize(body, stored) != stored:
                    fail(f"{path!r} chunk {k} not one frame")
                out = ctypes.create_string_buffer(len(raw))
                if z.ZSTD_decompress(out, len(raw), body, stored) != len(raw) or out.raw != raw:
                    fail(f"{path!r} chunk {k} does not decode to the file")
                stats["zstd"] += 1
            else:
                fail(f"{path!r} chunk {k} codec {codec}")
            frame = encode(raw, lvl)
            want_codecs.append(frame)
            if codec == 1 and frame != body:
                fail(f"{path!r} chunk {k}: not the frame ps5-dump-forge's call sequence makes")
        if flags & 1:
            fail(f"{path!r} is store-only")
        for k, frame in enumerate(want_codecs):
            (word,) = struct.unpack_from("<Q", data, chunks_off + 8 * (first + k))
            if (word >> 62) != (0 if frame is None else 1):
                fail(f"{path!r} chunk {k} codec choice")
        expect_chunk += count
    if expect_chunk != chunks or sum_sizes != total:
        fail("chunk count or logical total")
    if volumes and (volume + 1 != volumes or vol_sizes[volume] != cursor):
        fail("final volume size")
    print(
        f"ok    {packed}: {files} files, {chunks} chunks ({stats['zstd']} zstd, {stats['raw']} raw), "
        f"{volumes} volumes, level {lvl}"
    )


if __name__ == "__main__":
    main()
