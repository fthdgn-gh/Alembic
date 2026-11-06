#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Fake-signed SELFs: Alembic against ampr-emu-ng's tools/make_fself.py
# --ptype fake (what ps5-dump-forge's fself.rs matches), on synthetic PS5
# ELFs (make_elfs.py). Every SELF must be byte for byte the same, through
# `alembic fself` and through a conversion (folder -> folder and -> exFAT).
#   tools/fself-oracle/compare.sh [path/to/ampr-emu-ng]
# Needs a configured build in ./build and Python 3.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
ng="${1:-$repo/.refs/ampr-emu-ng}"
script="$ng/tools/make_fself.py"
[[ -f $script ]] || { echo "no make_fself.py at $script" >&2; exit 1; }
alembic="$repo/build/alembic"
[[ -x $alembic ]] || { echo "build Alembic first (cmake --build build)" >&2; exit 1; }
work="$(mktemp -d "${TMPDIR:-/tmp}/alembic-fself-oracle-XXXXXX")"
trap 'rm -rf "$work"' EXIT
python3 -I "$here/make_elfs.py" "$work/in"
fail=0
mkdir -p "$work/ref" "$work/ours" "$work/game/sce_sys" "$work/game/sce_module"
for elf in "$work"/in/*; do
    name="$(basename "$elf")"
    python3 -I "$script" --ptype fake "$elf" "$work/ref/$name" >/dev/null
    "$alembic" fself "$elf" "$work/ours/$name" >/dev/null
    if cmp -s "$work/ref/$name" "$work/ours/$name"; then echo "same  $name"; else echo "DIFF  $name"; fail=1; fi
done

# A game with them: eboot.bin and sce_module/<name>.prx, served fake-signed.
cp "$work/in/eboot.elf" "$work/game/eboot.bin"
for elf in "$work"/in/*; do
    name="$(basename "$elf")"
    [[ $name == eboot.elf ]] || cp "$elf" "$work/game/sce_module/${name%.*}.prx"
done
printf '{"titleId":"ABCD99999","contentVersion":"01.000.000"}' >"$work/game/sce_sys/param.json"
"$alembic" convert "$work/game" "$work/out/" >/dev/null
"$alembic" convert "$work/game" "$work/game.exfat" >/dev/null
"$alembic" convert "$work/game.exfat" "$work/from-exfat/" >/dev/null
for out in out from-exfat; do
    check() { if cmp -s "$1" "$2"; then echo "same  $out/$3"; else echo "DIFF  $out/$3"; fail=1; fi; }
    check "$work/ref/eboot.elf" "$work/$out/eboot.bin" eboot.bin
    for elf in "$work"/in/*; do
        name="$(basename "$elf")"
        [[ $name == eboot.elf ]] || check "$work/ref/$name" "$work/$out/sce_module/${name%.*}.prx" "sce_module/${name%.*}.prx"
    done
done
[[ $fail == 0 ]] && echo "all fself checks passed" || { echo "fself checks FAILED"; exit 1; }
