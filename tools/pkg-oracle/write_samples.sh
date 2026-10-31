#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Alembic's FPKG writer against ProsperoPkgTool on the owner's real packages
# in .refs/samples/: each PS5 package is unpacked to a folder, then built by
# both (img_create --seed S --no_right_sprx --compression stored|kraken and
# fpkg-build) and the two packages compared byte for byte.
#   tools/pkg-oracle/write_samples.sh [samples dir]
# Packages over ALEMBIC_SAMPLES_MAX_GB GiB (default 20) are skipped; games
# ProsperoPkgTool refuses (PS4 games, PS4-style PlayGo) are reported and
# skipped. Sample names are never printed: they are numbered.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
samples="${1:-$repo/.refs/samples}"
refs="$repo/.refs"
alembic="$repo/build/alembic"
max_gb="${ALEMBIC_SAMPLES_MAX_GB:-20}"
work="$(mktemp -d "${TMPDIR:-/tmp}/alembic-write-samples-XXXXXX")"
trap 'rm -rf "$work"' EXIT
keys="$refs/ProsperoPkgTool/ProsperoPkgTool/Keys"
seed=0102030405060708090a0b0c0d0e0f10

python3 -I "$here/patch_prosperopkgtool.py" "$refs/ProsperoPkgTool" >/dev/null || exit 1
dotnet build "$refs/ProsperoPkgTool/ProsperoPkgTool/ProsperoPkgTool.csproj" -c Release -nologo -v q -o "$work/ppt" >/dev/null || exit 1

fail=0 n=0
for f in "$samples"/*.pkg; do
    n=$((n + 1))
    size=$(wc -c <"$f" | tr -d ' ')
    if (( size > max_gb * 1073741824 )); then echo "skip  sample $n: over $max_gb GiB"; continue; fi
    rm -rf "$work/game"
    if ! "$alembic" convert "$f" "$work/game/" >/dev/null 2>&1 || [[ ! -f $work/game/sce_sys/param.json ]]; then
        echo "skip  sample $n: not a PS5 game"; rm -rf "$work/game"; continue
    fi
    for c in stored kraken; do
        if ! dotnet "$work/ppt/ProsperoPkgTool.dll" img_create "$work/game" --out "$work/ppt.pkg" --compression $c \
            --seed $seed --no_right_sprx >"$work/log" 2>&1; then
            echo "skip  sample $n $c: ProsperoPkgTool refuses it ($(grep -m1 -o -E '[A-Z_]+_INVALID|[A-Za-z.]+Exception' "$work/log"))"
            continue
        fi
        start=$SECONDS
        if ! "$alembic" fpkg-build "$work/game" "$work/alembic.pkg" --keys "$keys" --seed $seed --compression $c \
            --overwrite >"$work/log" 2>&1; then
            echo "FAIL  sample $n $c: $(tail -1 "$work/log")"; fail=1; continue
        fi
        if cmp -s "$work/ppt.pkg" "$work/alembic.pkg"; then
            echo "same  sample $n $c: byte for byte ($(wc -c <"$work/alembic.pkg" | tr -d ' ') bytes, $((SECONDS - start)) s)"
        else echo "DIFF  sample $n $c"; fail=1; fi
        rm -f "$work/ppt.pkg" "$work/alembic.pkg"
    done
    rm -rf "$work/game"
done
exit $fail
