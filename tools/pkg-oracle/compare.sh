#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds debug FPKGs with ProsperoPkgTool (stored and Kraken) and LibProsperoPKG,
# then checks Alembic's reader against them:
#   - FIH digest, CNT entry digests and outer superblock ICV verify,
#   - the decrypted pfs_image.dat equals ProsperoPkgTool's native-decrypt,
#   - the param.json CNT entry equals the source file.
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
    "$alembic" fpkg-dump "$pkg" entry:param.json "$work/$name.param.json"
    if cmp -s "$work/$name.param.json" "$game/sce_sys/param.json"; then echo "same  $name: param.json entry"
    else echo "DIFF  $name: param.json entry"; fail=1; fi
done
exit $fail
