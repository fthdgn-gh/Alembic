#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Checks Alembic's readers on real files in .refs/samples/ (git-ignored).
# For *.pkg (debug FPKG):
#   - every inner file equals ProsperoPkgTool's img_extract,
#   - the whole decoded inner image equals ProsperoPkgTool's --mount,
#   - Unity serialized files (if any) report their own sizes correctly,
#   - FPKG -> exFAT -> folder gives the same tree.
# For *.exfat:
#   - extraction equals PS5PKGTool's exFAT reader (tools/oracle),
#   - exFAT -> FFPFSC -> exFAT is byte-identical to the original.
# For *.ffpfsc:
#   - the SHA-256 of the decoded payload equals PS5PKGTool's verifier.
#   tools/pkg-oracle/check_samples.sh [passcode]
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
alembic="$repo/build/alembic"
passcode="${1:-00000000000000000000000000000000}"
work="$(mktemp -d "${TMPDIR:-/tmp}/alembic-samples-XXXXXX")"
trap 'rm -rf "$work"' EXIT
dotnet build "$repo/.refs/ProsperoPkgTool/ProsperoPkgTool/ProsperoPkgTool.csproj" -c Release -nologo -v q -o "$work/ppt" >/dev/null
ppt() { dotnet "$work/ppt/ProsperoPkgTool.dll" "$@"; }
dotnet build "$repo/tools/oracle/Oracle.csproj" -c Release -nologo -v q -p:PS5PKGToolDir="$repo/.refs/PS5PKGTool" \
    -o "$work/oracle" >/dev/null
oracle() { dotnet "$work/oracle/Oracle.dll" "$@"; }

fail=0
shopt -s nullglob
for pkg in "$repo"/.refs/samples/*.pkg; do
    name="$(basename "$pkg")"; w="$work/x"; rm -rf "$w"; mkdir -p "$w"
    echo "== $name"
    "$alembic" convert "$pkg" "$w/alembic/" --passcode "$passcode" >/dev/null 2>&1
    ppt img_extract "$pkg" "$w/ppt" --passcode "$passcode" >/dev/null
    if python3 -I "$here/../oracle/same_tree.py" "$w/ppt" "$w/alembic" --subset; then echo "same  every inner file"
    else echo "DIFF  inner files"; fail=1; fi
    "$alembic" fpkg-dump "$pkg" mount "$w/mount.img" --passcode "$passcode" | sed 's/^/      /'
    ppt img_extract "$pkg" "$w/pptm" --mount --passcode "$passcode" >/dev/null
    if cmp -s "$w/mount.img" "$(find "$w/pptm" -maxdepth 1 -type f | head -1)"; then echo "same  whole decoded inner image"
    else echo "DIFF  decoded inner image"; fail=1; fi
    rm -rf "$w/pptm" "$w/mount.img"
    set +e
    python3 -I - "$w/alembic" <<'PY'
import os, struct, sys
ok = bad = 0
for dp, dn, fn in os.walk(sys.argv[1]):
    for f in fn:
        # Serialized files only: *.assets, levelN (no extension) and the built-in ones;
        # levelN.resS / *.resource are raw streamed data with no header.
        if not (f.endswith('.assets') or (f.startswith('level') and '.' not in f) or f in ('globalgamemanagers', 'unity default resources', 'unity_builtin_extra')): continue
        p = os.path.join(dp, f); h = open(p, 'rb').read(48)
        v = struct.unpack('>I', h[8:12])[0]
        declared = struct.unpack('>Q', h[24:32])[0] if v >= 22 else struct.unpack('>I', h[4:8])[0]
        ok, bad = (ok + 1, bad) if declared == os.path.getsize(p) else (ok, bad + 1)
if ok or bad: print(f"{'same' if not bad else 'DIFF'}  Unity file headers: {ok} match, {bad} mismatch")
sys.exit(1 if bad else 0)
PY
    [[ $? -eq 0 ]] || fail=1
    set -e
    "$alembic" convert "$pkg" "$w/x.exfat" --passcode "$passcode" >/dev/null 2>&1
    "$alembic" convert "$w/x.exfat" "$w/from-exfat/" >/dev/null 2>&1
    if python3 -I "$here/../oracle/same_tree.py" "$w/alembic" "$w/from-exfat" --skip ampr_emu.index; then echo "same  FPKG -> exFAT -> folder"
    else echo "DIFF  FPKG -> exFAT -> folder"; fail=1; fi
done
for image in "$repo"/.refs/samples/*.exfat; do
    name="$(basename "$image")"; w="$work/x"; rm -rf "$w"; mkdir -p "$w"
    echo "== $name"
    "$alembic" convert "$image" "$w/alembic/" >/dev/null 2>&1
    oracle exfat-extract "$image" "$w/ps5pkgtool"
    if python3 -I "$here/../oracle/same_tree.py" "$w/ps5pkgtool" "$w/alembic"; then echo "same  extraction vs PS5PKGTool"
    else echo "DIFF  extraction vs PS5PKGTool"; fail=1; fi
    "$alembic" convert "$image" "$w/w.ffpfsc" --level 1 >/dev/null 2>&1
    "$alembic" convert "$w/w.ffpfsc" "$w/back.exfat" >/dev/null 2>&1
    if cmp -s "$image" "$w/back.exfat"; then echo "same  exFAT -> FFPFSC -> exFAT"
    else echo "DIFF  exFAT -> FFPFSC -> exFAT"; fail=1; fi
done
for container in "$repo"/.refs/samples/*.ffpfsc; do
    echo "== $(basename "$container")"
    ours="$("$alembic" ffpfsc-verify "$container" 2>/dev/null | awk '/payload sha256/ {print $3}')"
    theirs="$(oracle ffpfsc-verify "$container")"
    if [[ -n "$ours" && "$ours" == "$theirs" ]]; then echo "same  decoded payload SHA-256"
    else echo "DIFF  decoded payload SHA-256 ($ours vs $theirs)"; fail=1; fi
done
exit $fail
