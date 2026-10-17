#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Lets LibProsperoPKG run on macOS for testing.

.NET on macOS has no SHA3-256 (Apple's crypto lacks it; Linux .NET gets it
from OpenSSL), and LibProsperoPKG throws PlatformNotSupportedException. This
adds a managed SHA3-256 (ProsperoPkgTool's Keccak, MIT) as LibProsperoPkg.SHA3_256,
which C# name lookup prefers over System.Security.Cryptography.SHA3_256, and
rewrites the one IncrementalHash(SHA3_256) use. Hash results are identical; only
where they are computed changes. Idempotent.

usage: patch_libprosperopkg.py <LibProsperoPKG checkout> <ProsperoPkgTool checkout>
"""
import pathlib, re, sys

lib = pathlib.Path(sys.argv[1]) / "src" / "LibProsperoPkg"
ppt_sha3 = pathlib.Path(sys.argv[2]) / "ProsperoPkgTool" / "Crypto" / "Sha3.cs"
marker = "// alembic: managed SHA3 shim"

impl = ppt_sha3.read_text()
impl = impl.replace("namespace ProsperoPkgTool.Crypto;", "namespace LibProsperoPkg.AlembicShim;")
impl = impl.replace("public static class Sha3", "internal static class ManagedKeccak")
(lib / "AlembicManagedKeccak.cs").write_text(marker + " (from ProsperoPkgTool, MIT)\nusing System;\nusing System.IO;\n" + impl)

(lib / "AlembicSha3Shim.cs").write_text(f"""{marker}
using System;
using LibProsperoPkg.AlembicShim;
namespace LibProsperoPkg;

internal static class SHA3_256
{{
    public static bool IsSupported => true;
    public static byte[] HashData(byte[] data) => ManagedKeccak.Sha3_256(data);
    public static byte[] HashData(System.ReadOnlySpan<byte> data) => ManagedKeccak.Sha3_256(data);
    public static byte[] HashData(System.IO.Stream data) => ManagedKeccak.Sha3_256(data);
    public static int HashData(System.ReadOnlySpan<byte> data, System.Span<byte> destination)
    {{
        ManagedKeccak.Sha3_256(data).CopyTo(destination);
        return 32;
    }}
}}
""")

digest = lib / "PFS" / "Compression" / "ProsperoPfsDigest.cs"
text = digest.read_text()
old = re.search(r"using IncrementalHash hash = IncrementalHash\.CreateHash\(HashAlgorithmName\.SHA3_256\);\s*"
                r"hash\.AppendData\(header32\);\s*hash\.AppendData\(shuffleSection\);\s*"
                r"hash\.AppendData\(boundarySection\);\s*hash\.AppendData\(blockHashSection\);\s*"
                r"return hash\.GetHashAndReset\(\);", text)
if old:
    text = text.replace(old.group(0),
        "byte[] all = [.. header32, .. shuffleSection, .. boundarySection, .. blockHashSection]; " + marker + "\n"
        "        return SHA3_256.HashData(all);")
    digest.write_text(text)
elif marker not in text:
    sys.exit("ProsperoPfsDigest.cs changed upstream; update this patch")
print("patched", lib)
