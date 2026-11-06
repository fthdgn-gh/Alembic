#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes synthetic PS5 ELFs for the fake-signing oracle.

    make_elfs.py OUT_DIR

eboot.elf is ps5-dump-forge's own test ELF (fself.rs: a PT_LOAD of 0x4321
bytes with the process param, a RELRO, a PT_DYNAMIC, a PT_SCE_VERSION,
section headers at the end); make_fself.py makes an 18917-byte SELF of it.
The others reach the rest of the layout: a module (0xFE18) with every copied
segment type, a zero-size segment and two version segments; one with no
program headers; one whose program headers do not follow the ELF header;
one with a segment longer than a digest block; one with an ehsize larger
than its program headers.
"""
import os
import struct
import sys

ET_SCE_EXEC_ASLR, ET_SCE_DYNAMIC = 0xFE10, 0xFE18
PT_LOAD, PT_DYNAMIC, PT_SCE_DYNLIBDATA, PT_SCE_PROCPARAM = 1, 2, 0x61000000, 0x61000001
PT_SCE_RELRO, PT_SCE_COMMENT, PT_SCE_VERSION = 0x61000010, 0x6FFFFF00, 0x6FFFFF01


def elf(kind, size, phoff, phdrs, ehsize=0x40, fill=7, shoff=0, shnum=0):
    f = bytearray((i * fill + 3) & 0xFF for i in range(size))
    f[0:16] = b"\x7fELF" + bytes([2, 1, 1, 9, 0, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0x55])
    struct.pack_into("<HHIQQQIHHHHHH", f, 0x10, kind, 0x3E, 1, 0x1234, phoff, shoff, 0, ehsize, 0x38, len(phdrs), 0x40, shnum, 2)
    for i, (t, off, filesz) in enumerate(phdrs):
        struct.pack_into("<IIQQQQQQ", f, phoff + i * 0x38, t, 4, off, 0x100000 + off, 0x100000 + off, filesz, filesz + 0x100, 0x4000)
    return bytes(f)


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    forge = bytearray(elf(ET_SCE_EXEC_ASLR, 0x58C0, 0x40, [
        (PT_LOAD, 0x1000, 0x4321), (PT_SCE_PROCPARAM, 0x1200, 0x40), (PT_SCE_RELRO, 0x5400, 0x123),
        (PT_DYNAMIC, 0x5600, 0x80), (PT_SCE_VERSION, 0x5700, 0x25)], shoff=0x5800, shnum=3))
    struct.pack_into("<I", forge, 0x1200, 0x4942524F)  # the process param's magic, "ORBI"
    struct.pack_into("<I", forge, 0x120C, 0x04500031)
    files = {
        "eboot.elf": bytes(forge),
        "module.prx": elf(ET_SCE_DYNAMIC, 0x9000, 0x40, [
            (PT_LOAD, 0x400, 0x3001), (PT_SCE_DYNLIBDATA, 0x3500, 0x777), (PT_SCE_COMMENT, 0x4000, 0x10),
            (PT_LOAD, 0x4100, 0), (PT_SCE_VERSION, 0x4200, 0x30), (PT_SCE_RELRO, 0x4400, 0x200),
            (PT_SCE_VERSION, 0x4700, 0x18)], fill=13),
        "nophdr.elf": elf(ET_SCE_EXEC_ASLR, 0x200, 0x40, []),
        "late_phdr.elf": elf(ET_SCE_EXEC_ASLR, 0x3000, 0x200, [(PT_LOAD, 0x1000, 0x1800), (PT_SCE_RELRO, 0x2800, 0x100)], fill=5),
        "big_segment.elf": elf(ET_SCE_DYNAMIC, 0x30000, 0x40, [(PT_LOAD, 0x1000, 0x2C123)], fill=11),
        "big_ehsize.elf": elf(ET_SCE_EXEC_ASLR, 0x1000, 0x40, [(PT_LOAD, 0x400, 0x300)], ehsize=0x1F0, fill=3),
    }
    for name, data in files.items():
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)


if __name__ == "__main__":
    main()
