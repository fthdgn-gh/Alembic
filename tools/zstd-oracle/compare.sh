#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# zstd asset packs: Alembic against quer3q/ps5-dump-forge (its CLI, built from
# source with cargo).
#   tools/zstd-oracle/compare.sh [path/to/ps5-dump-forge] [--big] [--no-cargo]
#
# Building ps5-dump-forge runs its build scripts and downloads crates: the
# owner runs it (first passed 2026-11-02).
#
# ps5-dump-forge draws each pack's archive ID from getrandom. A scratch copy
# of the checkout gets a small patch (writer.rs: the ID from
# $FORGE_ZSTD_ARCHIVE_ID when set), is built in release mode, and packs a
# synthetic game (make_fixture.py) at several levels; Alembic packs the same
# folder with --zstd-archive-id. ampr_zstd.index and every volume must be byte
# for byte the same, and so must the set of loose files. Both outputs also go
# through check_pack.py (the format, every frame re-made with libzstd, the
# store-only rule) and through each tool's own reader.
# --no-cargo skips ps5-dump-forge and runs only the Alembic side and
# check_pack.py. Needs: a configured build in ./build, Python 3, libzstd
# 1.5.7 where ctypes finds it (Homebrew's), and for the comparison cargo
# (the toolchain ps5-dump-forge's rust-toolchain.toml names).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
forge="$repo/.refs/ps5-dump-forge"
big=()
cargo=1
for arg in "$@"; do
    case "$arg" in
    --big) big=(--big) ;;
    --no-cargo) cargo=0 ;;
    *) forge="$arg" ;;
    esac
done
alembic="$repo/build/alembic"
python="${PYTHON:-python3}"
export DYLD_FALLBACK_LIBRARY_PATH="${DYLD_FALLBACK_LIBRARY_PATH:-/opt/homebrew/lib}"
work="$(mktemp -d "${TMPDIR:-/tmp}/alembic-zstd-oracle-XXXXXX")"
trap 'rm -rf "$work"' EXIT
id=00112233445566778899aabbccddeeff
fail=0
# The original files, give or take the runtime the packer adds.
same_files() { # original, other, log
    "$alembic" same "$1" "$2" >"$3" 2>&1 || true
    ! grep -v -e '^only in b: fakelib/libSceAmpr.sprx$' -e ' files compared, ' "$3" | grep -q .
}
check() { # name, file a, file b
    if cmp -s "$2" "$3"; then echo "same  $1"; else echo "DIFF  $1"; cmp "$2" "$3" | head -1 || true; fail=1; fi
}

"$python" -I "$here/make_fixture.py" "$work/game" ${big[@]+"${big[@]}"}

# ps5-dump-forge, patched and built in a scratch copy (its target/ is reused
# between runs through CARGO_TARGET_DIR).
if [[ $cargo == 1 ]]; then
    [[ -f $forge/crates/ps5-dump-forge-zstd/src/writer.rs ]] || { echo "no ps5-dump-forge checkout at $forge" >&2; exit 1; }
    src="$work/forge-src"
    mkdir -p "$src"
    (cd "$forge" && tar --exclude ./target --exclude ./.git --exclude ./.claude -cf - .) | (cd "$src" && tar -xf -)
    "$python" -I - "$src/crates/ps5-dump-forge-zstd/src/writer.rs" <<'EOF'
import sys
p = sys.argv[1]
s = open(p).read()
old = '    getrandom::fill(&mut archive_id).map_err(|error| bad(format!("random archive ID: {error}")))?;\n'
new = '''    match std::env::var("FORGE_ZSTD_ARCHIVE_ID") {
        Ok(hex) if hex.len() == 32 => {
            for (i, byte) in archive_id.iter_mut().enumerate() {
                *byte = u8::from_str_radix(&hex[2 * i..2 * i + 2], 16)
                    .map_err(|_| bad("FORGE_ZSTD_ARCHIVE_ID: 32 hex digits"))?;
            }
        }
        _ => getrandom::fill(&mut archive_id)
            .map_err(|error| bad(format!("random archive ID: {error}")))?,
    }
'''
if s.count(old) != 1:
    sys.exit("writer.rs changed: the archive ID patch no longer applies")
open(p, "w").write(s.replace(old, new))
EOF
    export CARGO_TARGET_DIR="${CARGO_TARGET_DIR:-$repo/.refs/ps5-dump-forge-target}"
    (cd "$src" && cargo build --release --locked -p ps5-dump-forge-cli) >"$work/cargo.log" 2>&1 ||
        { tail -30 "$work/cargo.log"; exit 1; }
    forge_bin="$CARGO_TARGET_DIR/release/ps5-dump-forge"
    [[ -x $forge_bin ]] || { echo "no ps5-dump-forge binary at $forge_bin" >&2; exit 1; }
fi

# The runtime both trees carry: ps5-dump-forge installs the ampr-emu-ng release
# it embeds (vendor/ampr-emu-ng/libSceAmpr.sprx, see its README there); Alembic
# is given the same file so the loose files can be compared too. It is only
# copied into the temporary outputs.
runtime="$forge/vendor/ampr-emu-ng/libSceAmpr.sprx"
[[ -f $runtime ]] || { runtime="$work/runtime.sprx"; head -c 4096 /dev/zero >"$runtime"; }

for level in 5 1 3 19; do
    out="$work/alembic-$level"
    "$alembic" convert "$work/game" "$out/" --zstd --zstd-level "$level" --zstd-archive-id "$id" \
        --zstd-runtime "$runtime" --zstd-timestamp 1700000000 --threads 3 >"$work/log-$level" 2>&1 ||
        { cat "$work/log-$level"; exit 1; }
    if "$python" -I "$here/check_pack.py" "$out" "$work/game" --level "$level"; then :; else fail=1; fi
    "$alembic" zstd-verify "$out" >/dev/null && echo "ok    level $level alembic zstd-verify" ||
        { echo "FAIL  level $level alembic zstd-verify"; fail=1; }
    [[ $cargo == 1 ]] || continue
    fout="$work/forge-$level"
    FORGE_ZSTD_ARCHIVE_ID="$id" "$forge_bin" convert "$work/game" --to folder --zstd-pack --zstd-level "$level" \
        -o "$fout" >"$work/forge-$level.json" 2>"$work/forge-$level.log" || { tail -20 "$work/forge-$level.log"; exit 1; }
    check "level $level ampr_zstd.index" "$out/ampr_zstd.index" "$fout/ampr_zstd.index"
    count=0
    for vol in "$fout"/ampr_zstd-*.pak; do
        check "level $level $(basename "$vol")" "$out/$(basename "$vol")" "$vol"
        count=$((count + 1))
    done
    [[ $count == "$(find "$out" -maxdepth 1 -name 'ampr_zstd-*.pak' | wc -l | tr -d ' ')" ]] ||
        { echo "DIFF  level $level volume count"; fail=1; }
    # The same files left loose (ampr_emu.index is each tool's own).
    (cd "$out" && find . -type f ! -name ampr_emu.index | sort) >"$work/a.list"
    (cd "$fout" && find . -type f ! -name ampr_emu.index | sort) >"$work/f.list"
    if diff -q "$work/a.list" "$work/f.list" >/dev/null; then echo "same  level $level file list"; else
        echo "DIFF  level $level file list"; diff "$work/a.list" "$work/f.list" | head -10; fail=1; fi
    # Each reads the other's packs: Alembic verifies ps5-dump-forge's output,
    # and ps5-dump-forge unpacks Alembic's back to the original files.
    "$alembic" zstd-verify "$fout" >/dev/null && echo "ok    level $level alembic reads ps5-dump-forge's packs" ||
        { echo "FAIL  level $level alembic reading ps5-dump-forge's packs"; fail=1; }
    "$forge_bin" convert "$out" --to folder --zstd-unpack -o "$work/forge-unpacked-$level" >/dev/null 2>"$work/unpack-$level.log" ||
        { tail -20 "$work/unpack-$level.log"; fail=1; }
    same_files "$work/game" "$work/forge-unpacked-$level" "$work/same-$level" &&
        echo "same  level $level ps5-dump-forge unpacked == original" ||
        { echo "DIFF  level $level ps5-dump-forge unpacked"; head -5 "$work/same-$level"; fail=1; }
done

# Through images and back (Alembic only).
for image in packed.exfat packed.ffpkg packed.ffpfsc; do
    "$alembic" convert "$work/game" "$work/$image" --zstd --zstd-archive-id "$id" --zstd-runtime "$runtime" \
        --zstd-timestamp 1700000000 >/dev/null 2>&1
    "$alembic" convert "$work/$image" "$work/files-$image/" >/dev/null 2>&1
    check "$image ampr_zstd.index == folder's" "$work/files-$image/ampr_zstd.index" "$work/alembic-5/ampr_zstd.index"
    check "$image ampr_zstd-000.pak == folder's" "$work/files-$image/ampr_zstd-000.pak" "$work/alembic-5/ampr_zstd-000.pak"
    "$alembic" convert "$work/$image" "$work/unpacked-$image/" --zstd-unpack >/dev/null 2>&1
    same_files "$work/game" "$work/unpacked-$image" "$work/same-$image" &&
        echo "same  $image unpacked == original" || { echo "DIFF  $image unpacked"; head -5 "$work/same-$image"; fail=1; }
done

[[ $fail == 0 ]] && echo "all zstd checks passed" || { echo "zstd checks FAILED"; exit 1; }
