#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# The bench's icon: the app's icon0.png with red and blue swapped and an amber
# frame, so the development title never looks like Alembic on the home screen.
#   app/bench/icon.py IN.png OUT.png   (8-bit RGBA, not interlaced)
import struct, sys, zlib


def chunks(data):
    at = 8
    while at < len(data):
        size, kind = struct.unpack(">I4s", data[at:at + 8])
        yield kind, data[at + 8:at + 8 + size]
        at += 12 + size


def unfilter(raw, width, height, bpp=4):
    stride = width * bpp
    out, prev, at = bytearray(), bytearray(stride), 0
    for _ in range(height):
        f, line = raw[at], bytearray(raw[at + 1:at + 1 + stride])
        at += 1 + stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += line
        prev = line
    return out


def png(width, height, rgba):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))
    rows = b"".join(b"\0" + bytes(rgba[y * width * 4:(y + 1) * width * 4]) for y in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def main():
    data = open(sys.argv[1], "rb").read()
    header = dict(chunks(data))[b"IHDR"]
    width, height, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", header)
    assert (depth, color, interlace) == (8, 6, 0), "icon0.png must be 8-bit RGBA, not interlaced"
    raw = zlib.decompress(b"".join(body for kind, body in chunks(data) if kind == b"IDAT"))
    px = unfilter(raw, width, height)
    frame = width // 16
    for y in range(height):
        for x in range(width):
            i = (y * width + x) * 4
            if min(x, y, width - 1 - x, height - 1 - y) < frame:
                px[i:i + 4] = b"\xf2\xa2\x1c\xff"
            else:
                px[i], px[i + 2] = px[i + 2], px[i]
    open(sys.argv[2], "wb").write(png(width, height, px))


if __name__ == "__main__":
    main()
