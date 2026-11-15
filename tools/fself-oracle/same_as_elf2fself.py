#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Whether OURS (alembic fself --elf2fself) is REF (ps5-app-dumper's elf2fself)
# but for one thing: elf2fself writes what follows the last segment's data
# (the PT_SCE_VERSION bytes) right after it, where the SELF header's
# file_size, the place readers take them from, is that end 16-aligned. Ours
# puts them there, so between the two sit up to 15 zero bytes.
#   same_as_elf2fself.py REF OURS
import struct
import sys

ref = open(sys.argv[1], "rb").read()
ours = open(sys.argv[2], "rb").read()
header_size, meta_size = struct.unpack_from("<HH", ref, 0xC)
n = struct.unpack_from("<H", ref, 0x18)[0]
entries = (struct.unpack_from("<QQ", ref, 0x28 + 0x20 * i) for i in range(n))  # offset, size
end = max((offset + size for offset, size in entries), default=header_size + meta_size)
want = ref[:end] + bytes((-end) % 16) + ref[end:]
sys.exit(0 if ours == want else 1)
