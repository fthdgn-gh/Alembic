#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Checks Alembic's readers on real files in .refs/samples/ (git-ignored).
# For *.pkg (debug FPKG):
#   - every inner file equals ProsperoPkgTool's img_extract,
#   - the whole decoded inner image equals ProsperoPkgTool's --mount,
#   - Unity serialized files (if any) report their own sizes correctly,
#   - FPKG -> exFAT -> folder gives the same tree.
# For PS4 *.pkg (no third-party reader here yet):
#   - fpkg-info: entry digests, the PFS head digest and the superblock signature verify,
#   - the whole stored PFS hashes to the CNT header's digest (0x440),
#   - PKG -> exFAT -> folder and PKG -> FFPFSC -> folder give the same tree;
#   - with .refs/LibOrbisPkg's Keys.cs: the rebuilt PS4 package verifies, holds
#     the same files (param.sfo and the PlayGo files are made again), and
#     PKG -> exFAT -> package gives the same bytes as PKG -> package.
# For *.exfat:
#   - extraction equals PS5PKGTool's exFAT reader (tools/oracle),
#   - exFAT -> FFPFSC -> exFAT is byte-identical to the original.
# For *.ffpkg (UFS2):
#   - extraction equals PS5PKGTool's UFS2 reader,
#   - FFPKG -> FFPFSC -> FFPKG is byte-identical; FFPKG -> exFAT -> folder keeps every file.
# For *.ffpfsc:
#   - the SHA-256 of the decoded payload equals PS5PKGTool's verifier.
# Samples larger than ALEMBIC_SAMPLES_MAX_GB GiB (default 20) are skipped,
# and each step's output is deleted once it is compared, so the disk holds
# about two copies of one game at a time.
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

max_gb="${ALEMBIC_SAMPLES_MAX_GB:-20}"
# True (and says so) when the sample is over the size limit.
too_big() {
    local size; size=$(wc -c < "$1" | tr -d ' ')
    if (( size > max_gb * 1024 * 1024 * 1024 )); then
        echo "== $(basename "$1")"; echo "skip  $((size / 1024 / 1024 / 1024)) GiB, over the ${max_gb} GiB limit (ALEMBIC_SAMPLES_MAX_GB)"
        return 0
    fi
    return 1
}

fail=0
shopt -s nullglob
for pkg in "$repo"/.refs/samples/*.pkg; do
    too_big "$pkg" && continue
    name="$(basename "$pkg")"; w="$work/x"; rm -rf "$w"; mkdir -p "$w"
    echo "== $name"
    info="$("$alembic" fpkg-info "$pkg" --passcode "$passcode" 2>&1 || true)"
    if grep -q "^kind: *PS4 package" <<<"$info"; then
        if grep -q "^entry digests: *ok" <<<"$info" && grep -q "^PFS head digest: ok" <<<"$info" \
            && grep -q "ICV ok" <<<"$info"; then echo "same  entry digests, PFS head digest, superblock signature"
        else echo "DIFF  PS4 digests or signature:"; grep -E "digest|ICV" <<<"$info" | sed 's/^/      /'; fail=1; fi
        if python3 -I - "$pkg" <<'PY'
import hashlib, struct, sys
with open(sys.argv[1], 'rb') as f:
    h = f.read(0x480)
    offset, size = struct.unpack('>QQ', h[0x410:0x420])
    f.seek(offset); d = hashlib.sha256(); left = size
    while left:
        b = f.read(min(left, 1 << 24)); d.update(b); left -= len(b)
sys.exit(0 if d.digest() == h[0x440:0x460] else 1)
PY
        then echo "same  whole PFS digest"; else echo "DIFF  whole PFS digest"; fail=1; fi
        "$alembic" convert "$pkg" "$w/alembic/" --passcode "$passcode" >/dev/null 2>&1
        for via in exfat ffpfsc; do
            "$alembic" convert "$pkg" "$w/x.$via" --passcode "$passcode" >/dev/null 2>&1
            "$alembic" convert "$w/x.$via" "$w/from-$via/" >/dev/null 2>&1
            rm -f "$w/x.$via"
            if python3 -I "$here/../oracle/same_tree.py" "$w/alembic" "$w/from-$via"; then echo "same  PKG -> $via -> folder"
            else echo "DIFF  PKG -> $via -> folder"; fail=1; fi
            rm -rf "$w/from-$via"
        done
        ps4_keys="$repo/.refs/LibOrbisPkg/LibOrbisPkg/Util/Keys.cs"
        if [[ -f "$ps4_keys" ]]; then
            "$alembic" fpkg-build "$pkg" "$w/re.pkg" --ps4-keys "$ps4_keys" --passcode "$passcode" >/dev/null 2>&1
            info="$("$alembic" fpkg-info "$w/re.pkg" 2>&1 || true)"
            if grep -q "^entry digests: *ok" <<<"$info" && grep -q "^PFS head digest: ok" <<<"$info" \
                && grep -q "ICV ok" <<<"$info"; then echo "same  rebuilt PS4 package verifies"
            else echo "DIFF  rebuilt PS4 package does not verify"; fail=1; fi
            "$alembic" convert "$w/re.pkg" "$w/from-re/" >/dev/null 2>&1
            if python3 -I "$here/../oracle/same_tree.py" "$w/alembic" "$w/from-re" \
                --skip sce_sys/param.sfo --skip sce_sys/playgo-chunk.dat --skip sce_sys/playgo-chunk.sha \
                --skip sce_sys/playgo-manifest.xml; then echo "same  PKG -> PS4 package -> folder"
            else echo "DIFF  PKG -> PS4 package -> folder"; fail=1; fi
            rm -rf "$w/from-re"
            "$alembic" convert "$pkg" "$w/x.exfat" --passcode "$passcode" >/dev/null 2>&1
            "$alembic" convert "$w/x.exfat" "$w/re2.pkg" --ps4-keys "$ps4_keys" >/dev/null 2>&1
            if cmp -s "$w/re.pkg" "$w/re2.pkg"; then echo "same  PKG -> exFAT -> PS4 package equals PKG -> PS4 package"
            else echo "DIFF  PKG -> exFAT -> PS4 package"; fail=1; fi
        fi
        rm -rf "$w"
        continue
    fi
    "$alembic" convert "$pkg" "$w/alembic/" --passcode "$passcode" >/dev/null 2>&1
    ppt img_extract "$pkg" "$w/ppt" --passcode "$passcode" >/dev/null
    if python3 -I "$here/../oracle/same_tree.py" "$w/ppt" "$w/alembic" --subset; then echo "same  every inner file"
    else echo "DIFF  inner files"; fail=1; fi
    rm -rf "$w/ppt"
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
    rm -f "$w/x.exfat"
    if python3 -I "$here/../oracle/same_tree.py" "$w/alembic" "$w/from-exfat" --skip ampr_emu.index; then echo "same  FPKG -> exFAT -> folder"
    else echo "DIFF  FPKG -> exFAT -> folder"; fail=1; fi
    rm -rf "$w"
done
for image in "$repo"/.refs/samples/*.exfat; do
    too_big "$image" && continue
    name="$(basename "$image")"; w="$work/x"; rm -rf "$w"; mkdir -p "$w"
    echo "== $name"
    "$alembic" convert "$image" "$w/alembic/" >/dev/null 2>&1
    oracle exfat-extract "$image" "$w/ps5pkgtool"
    if python3 -I "$here/../oracle/same_tree.py" "$w/ps5pkgtool" "$w/alembic"; then echo "same  extraction vs PS5PKGTool"
    else echo "DIFF  extraction vs PS5PKGTool"; fail=1; fi
    rm -rf "$w/ps5pkgtool" "$w/alembic"
    "$alembic" convert "$image" "$w/w.ffpfsc" --level 1 >/dev/null 2>&1
    "$alembic" convert "$w/w.ffpfsc" "$w/back.exfat" >/dev/null 2>&1
    rm -f "$w/w.ffpfsc"
    if cmp -s "$image" "$w/back.exfat"; then echo "same  exFAT -> FFPFSC -> exFAT"
    else echo "DIFF  exFAT -> FFPFSC -> exFAT"; fail=1; fi
    rm -rf "$w"
done
for image in "$repo"/.refs/samples/*.ffpkg; do
    too_big "$image" && continue
    w="$work/x"; rm -rf "$w"; mkdir -p "$w"
    echo "== $(basename "$image")"
    "$alembic" convert "$image" "$w/alembic/" >/dev/null 2>&1
    oracle ufs2-extract "$image" "$w/ps5pkgtool"
    if python3 -I "$here/../oracle/same_tree.py" "$w/ps5pkgtool" "$w/alembic"; then echo "same  extraction vs PS5PKGTool"
    else echo "DIFF  extraction vs PS5PKGTool"; fail=1; fi
    rm -rf "$w/ps5pkgtool"
    "$alembic" convert "$image" "$w/w.ffpfsc" --level 1 >/dev/null 2>&1
    "$alembic" convert "$w/w.ffpfsc" "$w/back.ffpkg" >/dev/null 2>&1
    rm -f "$w/w.ffpfsc"
    if cmp -s "$image" "$w/back.ffpkg"; then echo "same  FFPKG -> FFPFSC -> FFPKG"
    else echo "DIFF  FFPKG -> FFPFSC -> FFPKG"; fail=1; fi
    rm -f "$w/back.ffpkg"
    "$alembic" convert "$image" "$w/x.exfat" >/dev/null 2>&1
    "$alembic" convert "$w/x.exfat" "$w/from-exfat/" >/dev/null 2>&1
    rm -f "$w/x.exfat"
    if python3 -I "$here/../oracle/same_tree.py" "$w/alembic" "$w/from-exfat" --skip ampr_emu.index; then
        echo "same  FFPKG -> exFAT -> folder"
    else echo "DIFF  FFPKG -> exFAT -> folder"; fail=1; fi
    rm -rf "$w"
done
for container in "$repo"/.refs/samples/*.ffpfsc; do
    too_big "$container" && continue
    echo "== $(basename "$container")"
    ours="$("$alembic" ffpfsc-verify "$container" 2>/dev/null | awk '/payload sha256/ {print $3}')"
    theirs="$(oracle ffpfsc-verify "$container")"
    if [[ -n "$ours" && "$ours" == "$theirs" ]]; then echo "same  decoded payload SHA-256"
    else echo "DIFF  decoded payload SHA-256 ($ours vs $theirs)"; fail=1; fi
done
exit $fail
