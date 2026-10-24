#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Pictures of the console app drawn on this computer (Mesa's software
# OpenGL, no window), against an engine here: by default the stand-in engine
# of the README screenshots (tools/screenshots/mock.mjs), started on port 5960.
#
#   app/preview.sh [out dir] [script]
#   ALEMBIC_ENGINE=127.0.0.1:5940 app/preview.sh out "shot:files"   # a real engine
#
# Script words: up down left right x o s t l1 r1 l2 r2 options, wait:<s>, shot:<name>.
# Needs Mesa (Linux: libegl-dev libgl-dev; macOS: brew install mesa) and, for
# the stand-in engine, Node and a PC build (build/alembic-engine).
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
kit="$repo/third_party/ps5-homebrew-ui"
app="$repo/app"
work="$repo/build-app"
out=${1:-$work/preview}
script=${2:-"shot:files r1 wait:1 shot:queue r1 wait:1 shot:deps r1 wait:1 shot:settings"}
mkdir -p "$work" "$out"
export PS5_CLANG=${PS5_CLANG:-$(command -v clang-18 || command -v clang || echo clang)}
source "$app/tools/host_env.sh"
source "$app/tools/fonts.sh"

mesa_flags=()
mesa_libs=(-lEGL -lGL)
if [[ $(uname -s) == Darwin ]]; then
    mesa=$(brew --prefix mesa 2>/dev/null) || { echo "brew install mesa" >&2; exit 2; }
    mesa_flags=(-I"$mesa/include")
    mesa_libs=(-L"$mesa/lib" -lEGL -lGL -Wl,-rpath,"$mesa/lib")
fi

source "$kit/tools/ninja-build.sh"
build="$work/preview-build"
ninja_begin "$build/build.ninja"
sources=("$kit/host/platform_host.cpp" "$repo/src/engine/json_value.cpp")
while IFS= read -r -d '' source; do sources+=("$source"); done < <(
    find "$kit/src" "$app/src" -type f \( -name '*.cpp' -o -name '*.c' \) \
        ! -path "$kit/src/platform/*" ! -path "$kit/src/runtime/*" ! -path "$app/src/main.cpp" -print0 | sort -z)
objects=()
for source in "${sources[@]}"; do
    relative=${source#"$repo/"}
    object="$build/obj/${relative//\//_}.o"
    if [[ $source == *.c ]]; then
        ninja_inputs=("$source")
        ninja_edge CC "$object" "$HOST_CC" -std=c11 -O2 -w -I"$kit/src" -MD -MF "$object.d" -c "$source" -o "$object"
    else
        ninja_inputs=("$source")
        ninja_edge CXX "$object" "$HOST_CXX" -std=c++20 -O2 -Wall -Wextra -fno-exceptions -fno-rtti \
            -DGL_GLEXT_PROTOTYPES=1 -DGL_SILENCE_DEPRECATION "${mesa_flags[@]}" -I"$kit/src" -I"$app/src" \
            -I"$repo/src/engine" -I"$repo/src/core/include" -MD -MF "$object.d" -c "$source" -o "$object"
    fi
    objects+=("$object")
done
ninja_inputs=("${objects[@]}")
ninja_edge LINK "$build/alembic_preview" "$HOST_CXX" "${objects[@]}" "${mesa_libs[@]}" -lm -o "$build/alembic_preview"
if ! ninja_run >"$build/build.log" 2>&1; then
    grep -E 'error|FAILED' -A6 "$build/build.log" | head -80 >&2
    exit 1
fi
grep -E 'warning' -A4 "$build/build.log" | grep -v third_party | head -40 >&2 || true

engine=${ALEMBIC_ENGINE:-}
if [[ -z $engine ]]; then
    # The stand-in engine, fed with a real engine's info and components.
    [[ -x $repo/build/alembic-engine ]] || { echo "build alembic-engine first (cmake --build build)" >&2; exit 1; }
    tmp=$(mktemp -d)
    trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$tmp"' EXIT
    mkdir "$tmp/state" "$tmp/root"
    "$repo/build/alembic-engine" --port 5961 --listen 127.0.0.1 --state "$tmp/state" --root "$tmp/root" >/dev/null 2>&1 &
    for _ in 1 2 3 4 5 6 7 8 9 10; do curl -sf http://127.0.0.1:5961/api/info >"$tmp/info.json" && break; sleep 0.5; done
    curl -sf http://127.0.0.1:5961/api/deps >"$tmp/deps.json"
    kill %1 2>/dev/null || true
    MOCK_FROZEN=${MOCK_FROZEN:-1} node "$repo/tools/screenshots/mock.mjs" "$repo" "$tmp" >/dev/null 2>&1 &
    for _ in 1 2 3 4 5 6 7 8 9 10; do curl -sf http://127.0.0.1:5960/api/info >/dev/null && break; sleep 0.3; done
    engine=127.0.0.1:5960
fi
EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe \
    "$build/alembic_preview" "$work/fonts" "$repo/assets/icon/icon0.png" "$out" "$engine" "$script"
