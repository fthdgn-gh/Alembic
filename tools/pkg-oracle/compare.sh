#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds debug FPKGs with ProsperoPkgTool (stored and Kraken) and LibProsperoPKG,
# then checks Alembic's reader against them:
#   - FIH digest, CNT entry digests and outer superblock ICV verify,
#   - the decrypted pfs_image.dat equals ProsperoPkgTool's native-decrypt,
#   - the param.json CNT entry equals the source file,
# and Alembic's writer against ProsperoPkgTool's:
#   - fpkg-build equals img_create --seed S --no_right_sprx byte for byte,
#     stored and Kraken, also with raw ELF modules (fake-signed by both);
#   - fself --prospero equals ProsperoPkgTool fself.
#   tools/pkg-oracle/compare.sh [refs-dir]   (default: .refs)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
refs="${1:-$repo/.refs}"
alembic="$repo/build/alembic"
work="$(mktemp -d "${TMPDIR:-/tmp}/alembic-pkg-oracle-XXXXXX")"
trap 'rm -rf "$work"' EXIT
passcode=00000000000000000000000000000000
cid=UP9999-PPSA09999_00-ALEMBICTEST00000

python3 -I "$here/patch_libprosperopkg.py" "$refs/LibProsperoPKG-sven" "$refs/ProsperoPkgTool" >/dev/null
dotnet build "$here/PkgOracle.csproj" -c Release -nologo -v q -p:LibProsperoPkgDir="$refs/LibProsperoPKG-sven" \
    -o "$work/lib" >/dev/null
dotnet build "$refs/ProsperoPkgTool/ProsperoPkgTool/ProsperoPkgTool.csproj" -c Release -nologo -v q -o "$work/ppt" >/dev/null
ppt() { dotnet "$work/ppt/ProsperoPkgTool.dll" "$@"; }
lib() { dotnet "$work/lib/PkgOracle.dll" "$@"; }

game="$work/game"
mkdir -p "$game/sce_sys" "$game/Data/Deep/Er"
cat > "$game/sce_sys/param.json" <<JSON
{"titleId":"PPSA09999","contentId":"$cid","applicationCategoryType":0,"applicationDrmType":"free","contentVersion":"01.000.000","masterVersion":"01.00","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Alembic Test"}}}
JSON
python3 -I - "$game" <<'PY'
import random, sys
g = sys.argv[1]; r = random.Random(7)
open(f"{g}/eboot.bin", "wb").write(r.randbytes(70000))
open(f"{g}/Data/text.txt", "wb").write(b"hello alembic " * 50000)
open(f"{g}/Data/noise.bin", "wb").write(r.randbytes(400000))
open(f"{g}/Data/zeros.bin", "wb").write(bytes(600000))
open(f"{g}/Data/empty.bin", "wb").write(b"")
open(f"{g}/Data/Deep/Er/small.txt", "wb").write(b"x" * 33)
words = [b"alembic ", b"kraken ", b"block ", b"\n", b'{"a": 1}, ', b"package "]
open(f"{g}/Data/words.txt", "wb").write(b"".join(r.choice(words) for _ in range(300000)))
PY

ppt img_create "$game" --out "$work/ppt-kraken.pkg" --compression kraken >/dev/null
ppt img_create "$game" --out "$work/ppt-stored.pkg" --compression stored >/dev/null
mkdir -p "$work/libout" && lib build "$game" "$work/libout" "$cid" >/dev/null
cp "$work"/libout/*.pkg "$work/lib.pkg"

fail=0
for name in ppt-kraken ppt-stored lib; do
    pkg="$work/$name.pkg"
    info="$("$alembic" fpkg-info "$pkg" 2>&1 || true)"
    if grep -q "FIH digest:      ok" <<<"$info" && grep -q "entry digests:   ok" <<<"$info" && grep -q "ICV ok" <<<"$info"; then
        echo "same  $name: digests and ICV verify"
    else
        echo "DIFF  $name: digest/ICV check"; echo "$info" | tail -5; fail=1
    fi
    ppt native-decrypt "$pkg" --passcode "$passcode" --out "$work/$name.ppt.bin" >/dev/null
    "$alembic" fpkg-dump "$pkg" inner-image "$work/$name.alembic.bin"
    if cmp -s "$work/$name.ppt.bin" "$work/$name.alembic.bin"; then echo "same  $name: decrypted inner image"
    else echo "DIFF  $name: decrypted inner image"; fail=1; fi
    case $name in
    ppt-*)  # FPKG -> folder must give back every source file (plus what the builder adds)
        "$alembic" convert "$pkg" "$work/$name-out/" >/dev/null 2>&1 || true
        if python3 -I "$here/../oracle/same_tree.py" "$game" "$work/$name-out" --subset; then
            echo "same  $name: FPKG -> folder gives back the source files"
        else echo "DIFF  $name: FPKG -> folder"; fail=1; fi ;;
    lib)    # uses NAPS shuffle, which no reader decodes yet: must be refused, not misread
        if "$alembic" ls "$pkg" >/dev/null 2>&1; then echo "DIFF  lib: shuffled layout was not refused"; fail=1
        else echo "ok    lib: shuffled NAPS layout refused as unsupported"; fi ;;
    esac
    "$alembic" fpkg-dump "$pkg" entry:param.json "$work/$name.param.json"
    if cmp -s "$work/$name.param.json" "$game/sce_sys/param.json"; then echo "same  $name: param.json entry"
    else echo "DIFF  $name: param.json entry"; fail=1; fi
done

# Writing. Alembic does not generate a keystone: the game takes the one
# ProsperoPkgTool generated above, so both tools build the same input.
keys="$refs/ProsperoPkgTool/ProsperoPkgTool/Keys"
seed=0102030405060708090a0b0c0d0e0f10
"$alembic" cat "$work/ppt-stored.pkg" sce_sys/keystone "$game/sce_sys/keystone"
for c in stored kraken; do
    ppt img_create "$game" --out "$work/w-ppt-$c.pkg" --compression $c --seed $seed --no_right_sprx >/dev/null
    "$alembic" fpkg-build "$game" "$work/w-alembic-$c.pkg" --keys "$keys" --seed $seed --compression $c >/dev/null
    if cmp -s "$work/w-ppt-$c.pkg" "$work/w-alembic-$c.pkg"; then
        echo "same  fpkg-build --compression $c: byte for byte ($(wc -c <"$work/w-alembic-$c.pkg" | tr -d ' ') bytes)"
    else echo "DIFF  fpkg-build --compression $c"; fail=1; fi
done
# Raw ELF modules: img_create fake-signs them (ProsperoPkgTool's flavour),
# eboot.bin's .sceversion record going to a stripped module; one it cannot
# sign is packed unchanged. Each also through ProsperoPkgTool fself alone.
elfgame="$work/elfgame"
cp -R "$game" "$elfgame"
python3 -I "$repo/tools/fself-oracle/make_elfs.py" --package-game "$elfgame"
for c in stored kraken; do
    ppt img_create "$elfgame" --out "$work/e-ppt-$c.pkg" --compression $c --seed $seed --no_right_sprx >/dev/null
    "$alembic" fpkg-build "$elfgame" "$work/e-alembic-$c.pkg" --keys "$keys" --seed $seed --compression $c \
        >"$work/e-alembic-$c.log" 2>&1
    if cmp -s "$work/e-ppt-$c.pkg" "$work/e-alembic-$c.pkg"; then
        echo "same  fpkg-build --compression $c with raw ELF modules: byte for byte"
    else echo "DIFF  fpkg-build --compression $c with raw ELF modules"; fail=1; fi
done
if grep -q "fake-signed: sce_module/libstripped.prx" "$work/e-alembic-stored.log" &&
    grep -qi "could not fake-sign data/tool.elf" "$work/e-alembic-stored.log"; then
    echo "ok    fpkg-build names what it signed and what it could not"
else echo "DIFF  fpkg-build's report"; cat "$work/e-alembic-stored.log"; fail=1; fi
python3 -I "$repo/tools/fself-oracle/make_elfs.py" "$work/elves"
for elf in "$work"/elves/* "$elfgame"/eboot.bin "$elfgame"/sce_module/lib*.prx "$elfgame"/fakelib/*.sprx \
    "$elfgame"/data/tool.elf "$elfgame"/data/level.bin; do
    n="$(basename "$elf")"
    if ppt fself "$elf" --out "$work/f-ppt-$n" >/dev/null 2>&1; then
        "$alembic" fself "$elf" "$work/f-alembic-$n" --prospero >/dev/null
        if cmp -s "$work/f-ppt-$n" "$work/f-alembic-$n"; then echo "same  fself --prospero $n"
        else echo "DIFF  fself --prospero $n"; fail=1; fi
    elif "$alembic" fself "$elf" "$work/f-alembic-$n" --prospero >/dev/null 2>&1; then
        echo "DIFF  fself --prospero $n: ProsperoPkgTool refuses it, Alembic does not"; fail=1
    else echo "ok    fself --prospero $n: refused by both"; fi
done
# A generated licence (--ps4-keys, LibOrbisPkg's Keys.cs): put in sce_sys,
# ProsperoPkgTool packs the same files into the same package.
ps4_keys="$refs/LibOrbisPkg/LibOrbisPkg/Util/Keys.cs"
if [[ -f $ps4_keys ]]; then
    "$alembic" fpkg-build "$game" "$work/l-alembic.pkg" --keys "$keys" --seed $seed --ps4-keys "$ps4_keys" >/dev/null
    for f in license.dat license.info; do "$alembic" fpkg-dump "$work/l-alembic.pkg" "entry:$f" "$game/sce_sys/$f"; done
    ppt img_create "$game" --out "$work/l-ppt.pkg" --compression stored --seed $seed --no_right_sprx >/dev/null
    if cmp -s "$work/l-ppt.pkg" "$work/l-alembic.pkg"; then echo "same  fpkg-build with a generated licence: byte for byte"
    else echo "DIFF  fpkg-build with a generated licence"; fail=1; fi
fi
exit $fail
