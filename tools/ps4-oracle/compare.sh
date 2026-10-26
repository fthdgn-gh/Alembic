#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# PS4 packages against maxton/LibOrbisPkg's PkgTool (.refs/LibOrbisPkg, built
# here for .NET 10):
#   - synthetic games (make_game.py): Alembic's package is byte-identical to
#     pkg_build's from the same GP4 project, and pkg_validate passes on it;
#   - PS4 packages in .refs/samples/: Alembic reads the same inner files as
#     pkg_extract, and rebuilding pkg_makegp4's project gives pkg_build's bytes.
#   tools/ps4-oracle/compare.sh [--large]
# Runs with TZ=UTC: LibOrbisPkg reads GP4 times as local time. Its PFSC
# reader is patched for .NET 6+ partial reads (see below).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
alembic="$repo/build/alembic"
lop="$repo/.refs/LibOrbisPkg"
keys="$lop/LibOrbisPkg/Util/Keys.cs"
export TZ=UTC LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8
work="$(mktemp -d "${TMPDIR:-/tmp}/alembic-ps4-XXXXXX")"
trap 'rm -rf "$work"' EXIT
[[ -f "$keys" ]] || { echo "no $keys: git clone https://github.com/maxton/LibOrbisPkg .refs/LibOrbisPkg" >&2; exit 1; }
# Quiet unless a step fails: then its output and where it stopped.
step() { local log="$work/step.log"; "$@" >"$log" 2>&1 || { echo "failed: $*" >&2; cat "$log" >&2; exit 1; }; }
# A copy retargeted to .NET 10 (a -p:TargetFramework would not reach the
# library PkgTool references); .refs/LibOrbisPkg stays as it is.
rsync -a --exclude obj --exclude bin --exclude .git "$lop/" "$work/src/"
for p in "$work/src/PkgTool.Core/PkgTool.Core.csproj" "$work/src/LibOrbisPkg.Core/LibOrbisPkg.Core.csproj"; do
    sed -i.bak 's#<TargetFramework>netcoreapp3.0</TargetFramework>#<TargetFramework>net10.0</TargetFramework>#' "$p"
done
# .NET 6 and later may return less from one DeflateStream.Read than asked
# for; LibOrbisPkg's PFSC reader (written for .NET Core 3.0) reads a block
# with one call. Read until the block is full, or extractions lose bytes.
python3 -I - "$work/src/LibOrbisPkg/PFS/PFSCReader.cs" <<'PY'
import sys
p = sys.argv[1]; s = open(p, encoding="utf-8-sig").read()
old = "ds.Read(output, 0, hdr.BlockSz);"
new = "int got = 0, n; while (got < hdr.BlockSz && (n = ds.Read(output, got, hdr.BlockSz - got)) > 0) got += n;"
if s.count(old) != 1: sys.exit("PFSCReader.cs: the DeflateStream read to patch is not there")
open(p, "w", encoding="utf-8").write(s.replace(old, new))
PY
step dotnet restore "$work/src/PkgTool.Core/PkgTool.Core.csproj"
step dotnet build "$work/src/PkgTool.Core/PkgTool.Core.csproj" -c Release -nologo --no-restore -o "$work/lop"
echo "built LibOrbisPkg's PkgTool"
pkgtool() { dotnet "$work/lop/PkgTool.Core.dll" "$@"; }
# pkg_validate: every check OK (secret-key checks are only warnings).
validate() {
    local out; out="$(pkgtool pkg_validate "$1" 2>&1 || true)"
    if grep -q "^\[ERROR\]" <<<"$out" || ! grep -q "^\[OK\]" <<<"$out"; then echo "$out" | grep -v "^\[OK\]" | sed 's/^/      /'; return 1; fi
}

fail=0
cases=(small media)
[[ "${1:-}" == "--large" ]] && cases+=(large)
for c in "${cases[@]}"; do
    w="$work/$c"; mkdir -p "$w"
    step python3 -I "$here/make_game.py" "$w" "$c"
    mkdir -p "$w/lop" # pkg_build does not make its output folder
    pkgtool pkg_build "$w/Project.gp4" "$w/lop" >"$w/lop.log" 2>&1 || true
    lop_pkg="$(find "$w/lop" -name '*.pkg' 2>/dev/null | head -1 || true)"
    [[ -n "$lop_pkg" ]] || { echo "DIFF  $c: pkg_build made nothing"; sed 's/^/      /' "$w/lop.log"; fail=1; continue; }
    step "$alembic" fpkg-build "$w/game" "$w/alembic.pkg" --ps4-keys "$keys"
    if cmp -s "$lop_pkg" "$w/alembic.pkg"; then echo "same  $c: package bytes"
    else echo "DIFF  $c: package bytes ($(cmp "$lop_pkg" "$w/alembic.pkg" | head -1))"; fail=1; fi
    if validate "$w/alembic.pkg"; then echo "same  $c: pkg_validate passes"; else echo "DIFF  $c: pkg_validate"; fail=1; fi
    rm -rf "$w"
done

shopt -s nullglob
for pkg in "$repo"/.refs/samples/*.pkg; do
    grep -q "^kind: *PS4 package" <<<"$("$alembic" fpkg-info "$pkg" 2>&1 || true)" || continue
    echo "== $(basename "$pkg")"
    w="$work/s"; rm -rf "$w"; mkdir -p "$w"
    pkgtool pkg_extract --passcode 00000000000000000000000000000000 "$pkg" "$w/lop" >>"$w/lop.log" 2>&1 || true
    "$alembic" convert "$pkg" "$w/alembic/" >/dev/null 2>&1
    # pkg_extract writes the inner PFS from its root: the game is under uroot/.
    if python3 -I "$repo/tools/oracle/same_tree.py" "$w/lop/uroot" "$w/alembic" --subset; then echo "same  every inner file vs pkg_extract"
    else echo "DIFF  inner files vs pkg_extract"; fail=1; fi
    rm -rf "$w/lop" "$w/alembic"
    pkgtool pkg_makegp4 --passcode 00000000000000000000000000000000 "$pkg" "$w/proj" >>"$w/lop.log" 2>&1 || true
    mkdir -p "$w/lop"
    pkgtool pkg_build "$w/proj/Project.gp4" "$w/lop" >>"$w/lop.log" 2>&1 || true
    lop_pkg="$(find "$w/lop" -name '*.pkg' 2>/dev/null | head -1 || true)"
    # The project's volume time (the image's), which Alembic cannot read from a folder.
    ts="$(python3 -I -c 'import re,sys,calendar,time; s=re.search(r"<volume_ts>([^<]+)<", open(sys.argv[1]).read()).group(1); print(calendar.timegm(time.strptime(s, "%Y-%m-%d %H:%M:%S")))' "$w/proj/Project.gp4")"
    # makegp4 moves param.sfo's creation date into the project's c_date.
    cdate="$(python3 -I -c 'import re,sys; m=re.search(r"c_date=\"([^\"]*)\"", open(sys.argv[1]).read()); print(m.group(1) if m else "")' "$w/proj/Project.gp4")"
    dated=()
    if [[ -n "$cdate" ]]; then
        dated=(--creation-date "$(tr -d - <<<"${cdate%% *}")")
        [[ "$cdate" == *" "* ]] && dated+=(--creation-time "$(tr -d : <<<"${cdate#* }")")
    fi
    rm -f "$w/proj/Project.gp4" # makegp4 puts the project inside the game folder
    "$alembic" fpkg-build "$w/proj" "$w/alembic.pkg" --ps4-keys "$keys" --timestamp "$ts" ${dated[@]+"${dated[@]}"} >/dev/null
    if [[ -n "$lop_pkg" ]] && cmp -s "$lop_pkg" "$w/alembic.pkg"; then echo "same  rebuilt from pkg_makegp4's project"
    elif [[ -n "$lop_pkg" ]]; then echo "DIFF  rebuilt from pkg_makegp4's project ($(cmp "$lop_pkg" "$w/alembic.pkg" | head -1))"; fail=1
    else echo "DIFF  rebuilt from pkg_makegp4's project: pkg_build made nothing"; sed 's/^/      /' "$w/lop.log" | tail -20; fail=1; fi
    if validate "$w/alembic.pkg"; then echo "same  pkg_validate passes"; else echo "DIFF  pkg_validate"; fail=1; fi
    rm -rf "$w"
done
exit $fail
