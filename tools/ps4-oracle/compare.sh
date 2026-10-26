#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# PS4 packages against maxton/LibOrbisPkg's PkgTool (.refs/LibOrbisPkg, built
# here for .NET 10):
#   - synthetic games (make_game.py): Alembic's package is byte-identical to
#     pkg_build's from the same GP4 project, and pkg_validate passes on it;
#   - PS4 packages in .refs/samples/: Alembic reads the same inner files as
#     pkg_extract, and rebuilding pkg_makegp4's project gives pkg_build's bytes.
#   tools/ps4-oracle/compare.sh [--large]
# Runs with TZ=UTC: LibOrbisPkg reads GP4 times as local time.
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
dotnet restore "$lop/PkgTool.Core/PkgTool.Core.csproj" -p:TargetFramework=net10.0 -v q >/dev/null
dotnet build "$lop/PkgTool.Core/PkgTool.Core.csproj" -c Release -nologo -v q --no-restore -p:TargetFramework=net10.0 \
    -o "$work/lop" >/dev/null
pkgtool() { dotnet "$work/lop/PkgTool.Core.dll" "$@"; }
# pkg_validate: every check OK (secret-key checks are only warnings).
validate() {
    local out; out="$(pkgtool pkg_validate "$1")"
    if grep -q "^\[ERROR\]" <<<"$out" || ! grep -q "^\[OK\]" <<<"$out"; then echo "$out" | grep -v "^\[OK\]" | sed 's/^/      /'; return 1; fi
}

fail=0
cases=(small media)
[[ "${1:-}" == "--large" ]] && cases+=(large)
for c in "${cases[@]}"; do
    w="$work/$c"; mkdir -p "$w"
    python3 -I "$here/make_game.py" "$w" "$c"
    pkgtool pkg_build "$w/Project.gp4" "$w/lop" >"$w/lop.log"
    lop_pkg="$(find "$w/lop" -name '*.pkg' | head -1)"
    [[ -n "$lop_pkg" ]] || { echo "DIFF  $c: pkg_build made nothing"; sed 's/^/      /' "$w/lop.log"; fail=1; continue; }
    "$alembic" fpkg-build "$w/game" "$w/alembic.pkg" --ps4-keys "$keys" >/dev/null
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
    pkgtool pkg_extract --passcode 00000000000000000000000000000000 "$pkg" "$w/lop" >/dev/null
    "$alembic" convert "$pkg" "$w/alembic/" >/dev/null 2>&1
    if python3 -I "$repo/tools/oracle/same_tree.py" "$w/lop" "$w/alembic" --subset; then echo "same  every inner file vs pkg_extract"
    else echo "DIFF  inner files vs pkg_extract"; fail=1; fi
    rm -rf "$w/lop" "$w/alembic"
    pkgtool pkg_makegp4 --passcode 00000000000000000000000000000000 "$pkg" "$w/proj" >/dev/null
    pkgtool pkg_build "$w/proj/Project.gp4" "$w/lop" >/dev/null
    lop_pkg="$(find "$w/lop" -name '*.pkg' | head -1)"
    # The project's volume time (the image's), which Alembic cannot read from a folder.
    ts="$(python3 -I -c 'import re,sys,calendar,time; s=re.search(r"<volume_ts>([^<]+)<", open(sys.argv[1]).read()).group(1); print(calendar.timegm(time.strptime(s, "%Y-%m-%d %H:%M:%S")))' "$w/proj/Project.gp4")"
    "$alembic" fpkg-build "$w/proj" "$w/alembic.pkg" --ps4-keys "$keys" --timestamp "$ts" >/dev/null
    if [[ -n "$lop_pkg" ]] && cmp -s "$lop_pkg" "$w/alembic.pkg"; then echo "same  rebuilt from pkg_makegp4's project"
    else echo "DIFF  rebuilt from pkg_makegp4's project"; fail=1; fi
    if validate "$w/alembic.pkg"; then echo "same  pkg_validate passes"; else echo "DIFF  pkg_validate"; fail=1; fi
    rm -rf "$w"
done
exit $fail
