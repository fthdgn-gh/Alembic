#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Round trip through every format with default options:
#   package -> P1 (fpkg-build, fixed seed) -> FFPFSC -> exFAT -> FFPKG -> FFPFS -> P2 (convert --to fpkg)
# P2 must equal P1 byte for byte, and every step must hold P1's files.
#   tools/pkg-oracle/roundtrip.sh <package.pkg> [keys dir] [work dir]
# The keys dir holds ProsperoPkgTool's passcode.bin and mount_image.bin
# (default: .refs/ProsperoPkgTool/ProsperoPkgTool/Keys).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
alembic="$repo/build/alembic"
package="$1"
keys="${2:-$repo/.refs/ProsperoPkgTool/ProsperoPkgTool/Keys}"
work="${3:-$(mktemp -d "${TMPDIR:-/tmp}/alembic-roundtrip-XXXXXX")}"
[[ -z ${3:-} ]] && trap 'rm -rf "$work"' EXIT
seed=0102030405060708090a0b0c0d0e0f10
fail=0

step() { # label, command...
    local label="$1" start=$SECONDS; shift
    if "$@" >"$work/log" 2>&1; then echo "ok    $label ($((SECONDS - start)) s)"; else echo "FAIL  $label"; cat "$work/log"; exit 1; fi
}
step "package -> P1" "$alembic" fpkg-build "$package" "$work/p1.pkg" --keys "$keys" --seed $seed
step "P1 -> FFPFSC" "$alembic" convert "$work/p1.pkg" "$work/c.ffpfsc"
step "FFPFSC -> exFAT" "$alembic" convert "$work/c.ffpfsc" "$work/c.exfat"
step "exFAT -> FFPKG" "$alembic" convert "$work/c.exfat" "$work/c.ffpkg"
step "FFPKG -> FFPFS" "$alembic" convert "$work/c.ffpkg" "$work/c.ffpfs"
step "FFPFS -> P2" "$alembic" convert "$work/c.ffpfs" "$work/p2.pkg" --keys "$keys" --seed $seed

for x in c.ffpfsc c.exfat c.ffpkg c.ffpfs p2.pkg; do
    result="$("$alembic" same "$work/p1.pkg" "$work/$x" 2>&1 | tail -1)"
    if [[ $result == *", 0 differences" ]] && ! "$alembic" same "$work/p1.pkg" "$work/$x" 2>&1 | grep -q '^only'; then
        echo "same  files of $x"
    else
        echo "DIFF  files of $x: $result"; fail=1
    fi
    # `same` ignores ampr_emu.index; the default keeps the game's own, so check it too.
    if "$alembic" ls "$work/p1.pkg" | grep -q ' ampr_emu.index$'; then
        rm -rf "$work/x"; "$alembic" convert "$work/$x" "$work/x/" >/dev/null 2>&1
        rm -rf "$work/y"; [[ -d $work/y ]] || "$alembic" convert "$work/p1.pkg" "$work/y/" >/dev/null 2>&1
        if cmp -s "$work/x/ampr_emu.index" "$work/y/ampr_emu.index"; then echo "same  ampr_emu.index of $x"; else echo "DIFF  ampr_emu.index of $x"; fail=1; fi
    fi
done
if cmp -s "$work/p1.pkg" "$work/p2.pkg"; then echo "same  P2 == P1 byte for byte"; else echo "DIFF  P2 != P1"; fail=1; fi
exit $fail
