#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds fixtures, runs Alembic and PS5PKGTool on each and compares the output.
# With an ampr_emu checkout, also compares Alembic's default AMPR index against
# ampr_emu's own builder (tools/build_ampr_index.py).
#   tools/oracle/compare.sh [path/to/PS5PKGTool] [path/to/ampr_emu]
# Needs: a configured build in ./build, dotnet 10, python3.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
ps5pkgtool="${1:-$repo/.refs/PS5PKGTool}"
ampr_emu="${2:-$repo/.refs/ampr_emu}"
alembic="$repo/build/alembic"
work="$(mktemp -d "${TMPDIR:-/tmp}/alembic-oracle-XXXXXX")"
trap 'rm -rf "$work"' EXIT

# PS5PKGTool's UFS2 writer, patched for a fixed clock, process ID and folder order.
python3 -I "$here/patch_ufs2.py" "$ps5pkgtool" "$work/ufs2-patched"
dotnet build "$here/Oracle.csproj" -c Release -nologo -v q -p:PS5PKGToolDir="$ps5pkgtool" \
    -p:Ufs2PatchedDir="$work/ufs2-patched" -o "$work/oracle" >/dev/null
oracle() { dotnet "$work/oracle/Oracle.dll" "$@"; }
python3 -I "$here/make_fixtures.py" "$work/fixtures"

fail=0
check() { # name, file a, file b
    if cmp -s "$2" "$3"; then echo "same  $1"; else echo "DIFF  $1"; cmp "$2" "$3" | head -1 || true; fail=1; fi
}

for fixture in "$work"/fixtures/*; do
    name="$(basename "$fixture")"
    for cluster in default 4096 131072; do
        extra=(--cluster-size ps5pkgtool); [[ $cluster != default ]] && extra=(--cluster-size "$cluster")
        oextra=(); [[ $cluster != default ]] && oextra=("$cluster")
        "$alembic" exfat "$fixture" "$work/$name-$cluster.alembic.img" --ampr-scheme ps5pkgtool ${extra[@]+"${extra[@]}"} 2>/dev/null
        oracle exfat "$fixture" "$work/$name-$cluster.oracle.img" ${oextra[@]+"${oextra[@]}"}
        check "exfat $name cluster=$cluster" "$work/$name-$cluster.alembic.img" "$work/$name-$cluster.oracle.img"
    done
done

# PS5PKGTool compresses at zlib level 9; Alembic defaults to 6, so parity runs pass --level 9.
# PFSC: compressed bytes depend on the zlib build on each side (they matched on
# macOS with .NET 10 and system zlib, 2026-10-17). The hard requirement is that
# each side decodes the other's output to the same bytes.
img="$work/many-default.oracle.img"
"$alembic" pfsc-encode "$img" "$work/a.pfsc" --level 9 >/dev/null 2>&1
oracle pfsc-encode "$img" "$work/o.pfsc" >/dev/null
oracle pfsc-decode "$work/a.pfsc" "$work/a.by-oracle"
"$alembic" pfsc-decode "$work/o.pfsc" "$work/o.by-alembic" 2>/dev/null
"$alembic" pfsc-decode "$work/a.pfsc" "$work/a.by-alembic" 2>/dev/null
check "pfsc alembic->oracle decode" "$work/a.by-oracle" "$work/a.by-alembic"
check "pfsc oracle->alembic decode" "$work/o.by-alembic" "$work/a.by-alembic"
head -c "$(stat -f%z "$img" 2>/dev/null || stat -c%s "$img")" "$work/a.by-alembic" > "$work/a.trimmed"
check "pfsc round trip" "$img" "$work/a.trimmed"
if cmp -s "$work/a.pfsc" "$work/o.pfsc"; then echo "same  pfsc container bytes"; else echo "note  pfsc container bytes differ (zlib builds differ; decode checks above still apply)"; fi

# FFPFSC: same fixed timestamp on both sides, then compare whole containers.
ts=1700000000
ffpfsc_case() { # name, source, extra alembic args...
    local name="$1" src="$2"; shift 2
    "$alembic" ffpfsc "$src" "$work/ff-$name.alembic" --timestamp $ts --level 9 --ampr-scheme ps5pkgtool --cluster-size ps5pkgtool "$@" >/dev/null 2>&1
    oracle ffpfsc-dir "$src" "$work/ff-$name.oracle" $ts >/dev/null
    check "ffpfsc $name" "$work/ff-$name.alembic" "$work/ff-$name.oracle"
}
for fixture in "$work"/fixtures/small "$work"/fixtures/ampr "$work"/fixtures/many "$work"/fixtures/empty "$work"/dumps/*; do
    ffpfsc_case "$(basename "$fixture" | tr -c 'A-Za-z0-9_\n-' _)" "$fixture"
done
"$alembic" ffpfsc "$work/many-default.oracle.img" "$work/ff-image.alembic" --timestamp $ts --level 9 >/dev/null 2>&1
oracle ffpfsc-image "$work/many-default.oracle.img" "$work/ff-image.oracle" $ts >/dev/null
check "ffpfsc from image" "$work/ff-image.alembic" "$work/ff-image.oracle"

# Reading PS5PKGTool's output back.
# FFPFSC -> folder must give back the original files.
for fixture in small ampr many large empty; do
    src="$work/fixtures/$fixture"
    oracle ffpfsc-dir "$src" "$work/rt-$fixture.ffpfsc" $ts >/dev/null
    "$alembic" convert "$work/rt-$fixture.ffpfsc" "$work/rt-$fixture/" >/dev/null 2>&1 || true
    if python3 -I "$here/same_tree.py" "$src" "$work/rt-$fixture" --skip ampr_emu.index; then
        echo "same  ffpfsc -> folder $fixture"
    else
        echo "DIFF  ffpfsc -> folder $fixture"; fail=1
    fi
done
# exFAT --rebuild must reproduce PS5PKGTool's image exactly (fixtures without an
# AMPR index: rebuilt files carry exFAT's fixed timestamps, which the index records).
for fixture in small many large empty; do
    img="$work/$fixture-default.oracle.img"
    "$alembic" convert "$img" "$work/rb-$fixture.exfat" --rebuild --ampr-scheme ps5pkgtool --cluster-size ps5pkgtool >/dev/null 2>&1
    check "exfat --rebuild $fixture" "$work/rb-$fixture.exfat" "$img"
done

# FFPKG (UFS2). PS5PKGTool writes names with Encoding.ASCII (non-ASCII becomes '?')
# and copies macOS junk files, so it is compared on ASCII-only copies without them.
export ALEMBIC_UFS2_TIME=1700000000 ALEMBIC_UFS2_PID=4242
for fixture in "$work"/fixtures/*; do
    name="$(basename "$fixture")"
    clean="$work/ufs2-src/$name"
    mkdir -p "$work/ufs2-src" && cp -R "$fixture" "$clean"
    { find "$clean" -depth | LC_ALL=C grep '[^ -~]' || true; } | while IFS= read -r p; do rm -rf "$p"; done
    find "$clean" \( -name .DS_Store -o -name '._*' \) -exec rm -rf {} +
    "$alembic" convert "$clean" "$work/$name.alembic.ffpkg" --ufs2-timestamp $ALEMBIC_UFS2_TIME \
        --ufs2-pid $ALEMBIC_UFS2_PID --volume-name TESTVOL >/dev/null 2>&1
    oracle ffpkg "$clean" "$work/$name.oracle.ffpkg" TESTVOL >/dev/null
    check "ffpkg $name" "$work/$name.alembic.ffpkg" "$work/$name.oracle.ffpkg"
done
# A size whose last cylinder group is smaller than its own metadata (the image
# then ends past fs_size, as PS5PKGTool's does).
mkdir -p "$work/ufs2-tail" && head -c $((39 * 409600 + 12345)) /dev/zero > "$work/ufs2-tail/f.bin"
"$alembic" convert "$work/ufs2-tail" "$work/tail.alembic.ffpkg" --ufs2-timestamp $ALEMBIC_UFS2_TIME \
    --ufs2-pid $ALEMBIC_UFS2_PID --volume-name T >/dev/null 2>&1
oracle ffpkg "$work/ufs2-tail" "$work/tail.oracle.ffpkg" T >/dev/null
check "ffpkg small last cylinder group" "$work/tail.alembic.ffpkg" "$work/tail.oracle.ffpkg"

# AMPR index: Alembic's default scheme against the console-side project's builder.
if [[ -f "$ampr_emu/tools/build_ampr_index.py" ]]; then
    fixture="$work/fixtures/amprclean"
    "$alembic" ampr-index "$fixture" "$work/alembic.index"
    python3 -I "$ampr_emu/tools/build_ampr_index.py" "$fixture" -o "$work/ampr_emu.index" >/dev/null 2>&1
    check "ampr index vs ampr_emu builder" "$work/alembic.index" "$work/ampr_emu.index"
    "$alembic" ampr-index "$fixture" "$work/ps5pkgtool.index" --ampr-scheme ps5pkgtool
    if cmp -s "$work/ps5pkgtool.index" "$work/ampr_emu.index"; then
        echo "note  ps5pkgtool scheme unexpectedly matches ampr_emu on non-ASCII names"
    else
        echo "ok    ps5pkgtool scheme differs from ampr_emu on non-ASCII names, as expected"
    fi
else
    echo "skip  ampr index vs ampr_emu builder (no ampr_emu checkout at $ampr_emu)"
fi

exit $fail
