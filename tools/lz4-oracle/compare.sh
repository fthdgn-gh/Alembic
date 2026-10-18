#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# LZ4 asset packs: Alembic against drakmor/ampr_emu's tools/ampr_pack.py.
#   tools/lz4-oracle/compare.sh [path/to/ampr_emu] [--big]
# Packs a synthetic game folder with Alembic, has ampr_pack.py pack the same
# folder with the configuration Alembic used (pack.toml + excludes.txt), and
# compares the manifest, its sidecars and every volume byte for byte. Then
# ampr_pack.py verifies Alembic's output, and the packed tree goes through
# exFAT and FFPFSC and back (--lz4-unpack) to the original files. Profiles:
# the unity and insomniac profiles (profiles/lz4/, or PROFILE_DIR) pack a game
# with that layout, compared against ampr_pack.py with the same TOML file, at
# its own level and with --lz4-level overriding it.
# Needs: a configured build in ./build, Python 3.11+ and LZ4 1.10.0, either the
# Python `lz4` module built against 1.10.0 or liblz4 1.10.0 where ctypes finds
# it (on macOS with Homebrew: DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib, set
# below). The Python module's bundled 1.9.x may give different HC output.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
ampr_emu="$repo/.refs/ampr_emu"
big=()
for arg in "$@"; do
    case "$arg" in
    --big) big=(--big) ;;
    *) ampr_emu="$arg" ;;
    esac
done
alembic="$repo/build/alembic"
python="${PYTHON:-$repo/.refs/venv/bin/python}"
[[ -x $python ]] || python=python3
export DYLD_FALLBACK_LIBRARY_PATH="${DYLD_FALLBACK_LIBRARY_PATH:-/opt/homebrew/lib}"
work="$(mktemp -d "${TMPDIR:-/tmp}/alembic-lz4-oracle-XXXXXX")"
trap 'rm -rf "$work"' EXIT
pack() { "$python" "$ampr_emu/tools/ampr_pack.py" "$@"; }

"$python" -I "$here/make_fixture.py" "$work/game" ${big[@]+"${big[@]}"}
fail=0
check() { # name, file a, file b
    if cmp -s "$2" "$3"; then echo "same  $1"; else echo "DIFF  $1"; cmp "$2" "$3" | head -1 || true; fail=1; fi
}

for variant in "level=9" "level=12 block=16384" "level=3 block=262144"; do
    args=(--lz4 --lz4-profile scan --lz4-timestamp 1700000500 --threads 3)
    for kv in $variant; do
        case "$kv" in
        level=*) args+=(--lz4-level "${kv#level=}") ;;
        block=*) args+=(--lz4-block-size "${kv#block=}") ;;
        esac
    done
    tag="${variant// /_}"
    out="$work/alembic-$tag"
    mkdir -p "$work/cfg-$tag"
    "$alembic" convert "$work/game" "$out/" "${args[@]}" --lz4-config-out "$work/cfg-$tag" >"$work/log-$tag" 2>&1 ||
        { cat "$work/log-$tag"; exit 1; }
    pack pack --root "$work/game" --ampr-index "$out/ampr_emu.index" --output "$work/py-$tag" \
        --config "$work/cfg-$tag/pack.toml" --exclude-from "$work/cfg-$tag/excludes.txt" --no-progress >"$work/py-$tag.json"
    check "$tag ampr_assets.index" "$out/ampr_assets.index" "$work/py-$tag/ampr_assets.index"
    check "$tag ampr_assets.index.crc" "$out/ampr_assets.index.crc" "$work/py-$tag/ampr_assets.index.crc"
    check "$tag ampr_assets.index.runtime" "$out/ampr_assets.index.runtime" "$work/py-$tag/ampr_assets.index.runtime"
    count=0
    for vol in "$work/py-$tag"/*.pak; do
        check "$tag $(basename "$vol")" "$out/$(basename "$vol")" "$vol"
        count=$((count + 1))
    done
    alembic_count=$(find "$out" -maxdepth 1 -name '*.pak' | wc -l | tr -d ' ')
    [[ $count == "$alembic_count" ]] || { echo "DIFF  $tag volume count $alembic_count vs $count"; fail=1; }
    if pack verify --index "$out/ampr_assets.index" >/dev/null; then echo "ok    $tag ampr_pack.py verify"; else echo "FAIL  $tag ampr_pack.py verify"; fail=1; fi
done

# Bundled profiles: Alembic reading the TOML vs ampr_pack.py reading it.
profile_dir="${PROFILE_DIR:-$repo/profiles/lz4}"
[[ -f $profile_dir/unity.toml ]] || profile_dir="$repo/.refs/PS5-LZ4-Compression/profiles"
compare_sets() { # tag, alembic folder, ampr_pack.py folder
    local tag="$1" a="$2" p="$3" vol count=0
    for f in ampr_assets.index ampr_assets.index.crc; do check "$tag $f" "$a/$f" "$p/$f"; done
    if [[ -f $p/ampr_assets.index.runtime || -f $a/ampr_assets.index.runtime ]]; then
        check "$tag ampr_assets.index.runtime" "$a/ampr_assets.index.runtime" "$p/ampr_assets.index.runtime"
    fi
    for vol in "$p"/*.pak; do
        check "$tag $(basename "$vol")" "$a/$(basename "$vol")" "$vol"
        count=$((count + 1))
    done
    [[ $count -gt 0 && $count == "$(find "$a" -maxdepth 1 -name '*.pak' | wc -l | tr -d ' ')" ]] ||
        { echo "DIFF  $tag volume count"; fail=1; }
}
for engine in unity insomniac; do
    toml="$profile_dir/$engine.toml"
    [[ -f $toml ]] || { echo "skip  $engine profile (no $toml)"; continue; }
    "$python" -I "$here/make_fixture.py" "$work/$engine-game" --layout "$engine"
    # The profile's own level (12), then --lz4-level 9 replacing it.
    mkdir -p "$work/$engine-cfg"
    sed 's/^\[pack\]$/[pack]\ncompression_level = 9/' "$toml" > "$work/$engine-cfg/level9.toml"
    for level in 12 9; do
        tag="$engine-level$level"
        config="$toml"; [[ $level == 9 ]] && config="$work/$engine-cfg/level9.toml"
        out="$work/alembic-$tag"
        mkdir -p "$work/cfg-$tag"
        "$alembic" convert "$work/$engine-game" "$out/" --lz4 --lz4-profile "$toml" --lz4-level $level \
            --lz4-timestamp 1700000500 --lz4-config-out "$work/cfg-$tag" >"$work/log-$tag" 2>&1 ||
            { cat "$work/log-$tag"; exit 1; }
        pack pack --root "$work/$engine-game" --ampr-index "$out/ampr_emu.index" --output "$work/py-$tag" \
            --config "$config" --exclude-from "$work/cfg-$tag/excludes.txt" --no-progress >"$work/py-$tag.json"
        compare_sets "$tag" "$out" "$work/py-$tag"
    done
    # What the profile kept loose is still a loose file.
    if [[ $engine == unity ]]; then
        out="$work/alembic-unity-level12"
        for f in Media/globalgamemanagers Media/level0 Media/Metadata/global-metadata.dat; do
            if [[ -f $out/$f ]]; then echo "ok    unity keeps $f loose"; else echo "FAIL  unity packed $f"; fail=1; fi
        done
        if [[ ! -f $out/Media/StreamingAssets/aa/PS5/level_assets_all.bundle ]]; then echo "ok    unity packs StreamingAssets"; else
            echo "FAIL  unity left StreamingAssets loose"; fail=1; fi
    fi
done

# ampr_emu's own index builder agrees with the index the packs refer to.
"$python" "$ampr_emu/tools/build_ampr_index.py" "$work/game" --output "$work/py.index" >/dev/null 2>&1 ||
    "$python" "$ampr_emu/tools/build_ampr_index.py" "$work/game" >/dev/null
[[ -f $work/py.index ]] || cp "$work/game/ampr_emu.index" "$work/py.index"
check "ampr_emu.index vs build_ampr_index.py" "$work/alembic-level=9/ampr_emu.index" "$work/py.index"

# Through images and back.
out="$work/alembic-level=9"
"$alembic" convert "$work/game" "$work/packed.exfat" --lz4 --lz4-timestamp 1700000500 >/dev/null 2>&1
"$alembic" convert "$work/game" "$work/packed.ffpfsc" --lz4 --lz4-timestamp 1700000500 >/dev/null 2>&1
for image in packed.exfat packed.ffpfsc; do
    if "$alembic" lz4-verify "$work/$image" >/dev/null; then echo "ok    lz4-verify $image"; else echo "FAIL  lz4-verify $image"; fail=1; fi
    "$alembic" convert "$work/$image" "$work/files-$image/" >/dev/null 2>&1
    if diff -rq "$out" "$work/files-$image" >/dev/null; then echo "same  $image extracted == folder output"; else
        echo "DIFF  $image extracted vs folder output"; diff -rq "$out" "$work/files-$image" | head -5; fail=1; fi
    "$alembic" convert "$work/$image" "$work/unpacked-$image/" --lz4-unpack >/dev/null 2>&1
    # The original plus the generated ampr_emu.index.
    extra=$(diff -rq "$work/game" "$work/unpacked-$image" | grep -v "Only in $work/unpacked-$image: ampr_emu.index" || true)
    if [[ -z $extra ]]; then echo "same  $image unpacked == original"; else echo "DIFF  $image unpacked"; echo "$extra" | head -5; fail=1; fi
done

[[ $fail == 0 ]] && echo "all LZ4 checks passed" || { echo "LZ4 checks FAILED"; exit 1; }
