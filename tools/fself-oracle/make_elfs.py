#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes synthetic PS5 ELFs for the fake-signing oracle.

    make_elfs.py OUT_DIR
    make_elfs.py --package-game GAME_DIR


eboot.elf is ps5-dump-forge's own test ELF (fself.rs: a PT_LOAD of 0x4321
bytes with the process param, a RELRO, a PT_DYNAMIC, a PT_SCE_VERSION,
section headers at the end); make_fself.py makes an 18917-byte SELF of it.
The others reach the rest of the layout: a module (0xFE18) with every copied
segment type, a zero-size segment and two version segments; one with no
program headers; one whose program headers do not follow the ELF header;
one with a segment longer than a digest block; one with an ehsize larger
than its program headers.

--package-game writes the executables of a game for ProsperoPkgTool's
flavour (what img_create does to a package's modules): ELFs with section
tables, an eboot.bin with a .sceversion section and 0x40 at 0x3F00, a
stripped module (it takes eboot.bin's record), a module with its own
record, a fakelib module with 0x80 at 0x3F00, one img_create cannot sign
(its program headers do not follow the ELF header), an ELF under a data
name and a module that is already a SELF (both packed unchanged).
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


def sceversion(name, version):
    """A .sceversion record as the SDK writes one: length, 8, "name:", the tuple twice."""
    n = name.encode() + b":"
    size = 1 + len(n) + 16
    return struct.pack("<HHB", 0, size, 8) + n + version + version


def with_sections(data, sections, ex_info=None):
    """`data` with section payloads, a .shstrtab and a section table appended."""
    f = bytearray(data)
    if ex_info is not None:
        f[0x3F00] = ex_info
    names = b"\0.shstrtab\0" + b"".join(n.encode() + b"\0" for n, _ in sections)
    placed = []
    for name, payload in [(".shstrtab", names)] + sections:
        placed.append((len(f), len(payload)))
        f += payload
    while len(f) % 8:
        f.append(0)
    shoff = len(f)
    f += bytes(0x40)  # the null section
    name_at = 1
    for (offset, size), name in zip(placed, [".shstrtab"] + [n for n, _ in sections]):
        f += struct.pack("<IIQQQQIIQQ", name_at, 3 if name == ".shstrtab" else 1, 0, 0, offset, size, 0, 0, 1, 0)
        name_at += len(name) + 1
    struct.pack_into("<Q", f, 0x28, shoff)
    struct.pack_into("<HH", f, 0x3C, 1 + len(placed), 1)  # e_shnum, e_shstrndx
    struct.pack_into("<H", f, 0x3A, 0x40)
    return bytes(f)


def package_game(game):
    v = bytes.fromhex("0450003100000411")
    eboot = with_sections(elf(ET_SCE_EXEC_ASLR, 0x5000, 0x40, [
        (PT_LOAD, 0x1000, 0x2345), (PT_SCE_RELRO, 0x3400, 0x300), (PT_SCE_DYNLIBDATA, 0x3800, 0x5A0)], fill=17),
        [(".sceversion", sceversion("eboot", v) + sceversion("libc", v))], ex_info=0x40)
    files = {
        "eboot.bin": eboot,
        "sce_module/libstripped.prx": elf(ET_SCE_DYNAMIC, 0x2000, 0x40, [(PT_LOAD, 0x400, 0x1234), (PT_SCE_COMMENT, 0x1800, 0x40)], fill=19),
        "sce_module/libown.prx": with_sections(elf(ET_SCE_DYNAMIC, 0x2400, 0x40, [(PT_LOAD, 0x400, 0x1F00)], fill=23),
                                               [(".sceversion", sceversion("libown", bytes.fromhex("0300001100000001")))]),
        "fakelib/libSceFoo.sprx": elf(ET_SCE_DYNAMIC, 0x4000, 0x40, [(PT_LOAD, 0x800, 0x3000)], fill=29)[:0x3F00] + b"\x80" + bytes(0xFF),
        "data/tool.elf": elf(ET_SCE_EXEC_ASLR, 0x1000, 0x100, [(PT_LOAD, 0x400, 0x200)], fill=31),
        "data/level.bin": elf(ET_SCE_DYNAMIC, 0x800, 0x40, [(PT_LOAD, 0x200, 0x300)], fill=37),
        "sce_module/already.prx": b"\x4f\x15\x3d\x1d" + bytes(0x3C) + bytes(range(256)) * 8,
    }
    for name, data in files.items():
        path = os.path.join(game, name)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)


def main():
    if sys.argv[1] == "--package-game":
        return package_game(sys.argv[2])
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
