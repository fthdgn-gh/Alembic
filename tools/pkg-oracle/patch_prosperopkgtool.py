#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lets ProsperoPkgTool build packages over 1 GiB on macOS for testing.

.NET on macOS has no SHA3-256, and img_create's file-backed SI path hashes the
mount image with IncrementalHash(SHA3_256), which throws there. This rewrites
that one use to ProsperoPkgTool's own managed Keccak (Crypto/Sha3.cs,
Sha3StreamState). Hash results are identical; only where they are computed
changes. Idempotent.

usage: patch_prosperopkgtool.py <ProsperoPkgTool checkout>
"""
import pathlib, re, sys

path = pathlib.Path(sys.argv[1]) / "ProsperoPkgTool" / "Containers" / "ProsperoSiArchive.cs"
marker = "// alembic: managed SHA3"
text = path.read_text()
if marker in text:
    print("already patched", path)
    sys.exit(0)
old = re.search(r"using var sha = System\.Security\.Cryptography\.IncrementalHash\.CreateHash\(\s*"
                r"System\.Security\.Cryptography\.HashAlgorithmName\.SHA3_256\);", text)
if not old:
    sys.exit("ProsperoSiArchive.cs changed upstream; update this patch")
text = text.replace(old.group(0), "var sha = new ProsperoPkgTool.Crypto.Sha3.Sha3StreamState(); " + marker)
text = text.replace("sha.AppendData(buffer.AsSpan(0, read));", "sha.Append(buffer.AsSpan(0, read));")
text = text.replace("return (sha.GetHashAndReset(), crc);", "return (sha.Finish(), crc);")
path.write_text(text)
print("patched", path)
