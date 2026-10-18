#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Copies PS5PKGTool's UFS2 sources (SvenGDK/UFS2Tool, BSD-2-Clause) into
OUT_DIR under the namespace UFS2ToolPatched, with three changes so the oracle
is reproducible: the clock and the process ID come from ALEMBIC_UFS2_TIME and
ALEMBIC_UFS2_PID, and folder listings are sorted case-insensitively (the
order NTFS gives PS5PKGTool on Windows, and the one Alembic uses).
  patch_ufs2.py <PS5PKGTool checkout> <out dir>"""
import pathlib
import sys

src = pathlib.Path(sys.argv[1]) / "PS5PKGTool.Ufs2"
out = pathlib.Path(sys.argv[2])
out.mkdir(parents=True, exist_ok=True)
listing = 'new DirectoryInfo(dirPath).EnumerateFileSystemInfos("*", SearchOption.TopDirectoryOnly)'
for path in sorted(src.glob("*.cs")):
    text = path.read_bytes().decode("latin-1")  # keeps every byte as it is
    text = text.replace("namespace UFS2Tool", "namespace UFS2ToolPatched")
    if path.name == "Ufs2ImageCreator.cs":
        for old, new in [
            ("DateTimeOffset.UtcNow.ToUnixTimeSeconds()", "OracleOverrides.Now"),
            ("Environment.ProcessId", "OracleOverrides.Pid"),
            (listing, f"OracleOverrides.Sorted({listing})"),
        ]:
            if old not in text:
                sys.exit(f"patch_ufs2.py: '{old}' not found in {path.name}; PS5PKGTool changed")
            text = text.replace(old, new)
    (out / path.name).write_bytes(text.encode("latin-1"))
(out / "OracleOverrides.cs").write_text('''namespace UFS2ToolPatched
{
    internal static class OracleOverrides
    {
        public static long Now => long.Parse(Environment.GetEnvironmentVariable("ALEMBIC_UFS2_TIME") ?? "1700000000");
        public static int Pid => int.Parse(Environment.GetEnvironmentVariable("ALEMBIC_UFS2_PID") ?? "0");
        public static IEnumerable<FileSystemInfo> Sorted(IEnumerable<FileSystemInfo> entries) =>
            entries.OrderBy(e => e.Name, StringComparer.OrdinalIgnoreCase).ThenBy(e => e.Name, StringComparer.Ordinal).ToList();
    }
}
''', encoding="utf-8")
