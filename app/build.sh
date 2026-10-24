#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds Alembic's console app (a native PS5 title drawn with ps5-homebrew-ui)
# into build-app/dist/<TITLE_ID> and a ZIP of it, for ShadowMount+'s
# /data/homebrew. Adapted from ps5-homebrew-ui's tools/build.sh (GPL-3.0).
#
#   app/build.sh
#
# Needs clang 18 (Homebrew's llvm@18 on a Mac), ninja, python3, curl, unzip
# and, on a Mac, GNU coreutils and findutils (Homebrew). The PS5 payload SDK
# v0.42, zlib, the ps5-opengl SDK and (on a Mac) clang's x86-64 builtins are
# downloaded into build-app/ and checked against pinned SHA-256s.
# ALEMBIC_ENGINE_ELF=FILE puts that engine payload into the app, which sends
# it to the ELF loader when no engine answers (default: build-ps5/alembic.elf
# when it exists).
set -euo pipefail

repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
kit="$repo/third_party/ps5-homebrew-ui"
app="$repo/app"
work="$repo/build-app"
export HUI_WORK="$work/hui"
mkdir -p "$work" "$HUI_WORK"

source "$app/tools/host_env.sh"

param="$app/sce_sys/param.json"
title_id=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["titleId"])' "$param")
[[ $title_id =~ ^[A-Z]{4}[0-9]{5}$ ]] || { echo "bad titleId in $param" >&2; exit 2; }

# Loader and container constants the kit validated on firmware 6.02 and 12.70.
module_sdk=0x02000009
companion_sdk=0x08050001
fself_magic=0x1D3D154F

bash "$kit/tools/validate-assets.sh" "$app/sce_sys"
bash "$kit/tools/prepare-opengl.sh" >/dev/null
sdk_root="$HUI_WORK/.deps/native/ps5-payload-sdk"
opengl="$HUI_WORK/.deps/ps5-opengl/current"
build="$work/build"
tool="$HUI_WORK/build/host/ps5-native-tool"
[[ -f $HUI_WORK/runtime/libc.prx ]] || bash "$kit/tools/rebuild-libc.sh" >/dev/null
bash "$kit/tools/build-host-tools.sh"
mkdir -p "$build/obj"

source "$kit/tools/ninja-build.sh"
ninja_begin "$build/app.ninja"
target_compiler=$(command -v "$PS5_CLANG")
wrapper="$kit/tooling/prospero-clang18"

# The kit (minus its demo app), Alembic's app, and the engine's JSON parser.
sources=()
while IFS= read -r -d '' source; do sources+=("$source"); done < <(
    find "$kit/src" "$app/src" -type f \( -name '*.c' -o -name '*.cpp' \) \
        ! -path "$app/src/host/*" -print0 | sort -z)
sources+=("$repo/src/engine/json_value.cpp")

includes=(-I"$kit/src" -I"$app/src" -I"$repo/src/engine" -I"$repo/src/core/include" -I"$opengl/include")
objects=()
for source in "${sources[@]}"; do
    relative=${source#"$repo/"}
    object="$build/obj/${relative//\//_}.o"
    if [[ $source == *.c ]]; then standard=-std=c11; else standard=-std=c++20; fi
    args=("$standard" -O2 -Wall -Wextra -ffunction-sections -fdata-sections -DGL_GLEXT_PROTOTYPES=1)
    [[ $source == *.c ]] || args+=(-fno-exceptions -fno-rtti)
    # Vendored upstream code is built as published.
    [[ $source != "$kit/src/third_party/"* ]] || args+=(-w)
    ninja_inputs=("$source" "$wrapper" "$target_compiler")
    ninja_edge CC "$object" env PS5_PAYLOAD_SDK="$sdk_root" PS5_CLANG="$target_compiler" \
        USE_CCACHE="${USE_CCACHE:-0}" sh "$wrapper" "${args[@]}" "${includes[@]}" \
        -MD -MF "$object.d" -c "$source" -o "$object"
    objects+=("$object")
done
native="$kit/tooling/native"
for name in app_crt app_cpp_runtime; do
    object="$build/obj/$name.o"
    ninja_inputs=("$native/$name.cpp" "$wrapper" "$target_compiler")
    ninja_edge CXX "$object" env PS5_PAYLOAD_SDK="$sdk_root" PS5_CLANG="$target_compiler" \
        USE_CCACHE="${USE_CCACHE:-0}" sh "$wrapper" -std=c++20 -O2 -Wall -Wextra \
        -fno-exceptions -fno-rtti -ffunction-sections -fdata-sections \
        -MD -MF "$object.d" -c "$native/$name.cpp" -o "$object"
done

link_inputs=("$build/obj/app_crt.o" "$build/obj/app_cpp_runtime.o" "${objects[@]}"
    "$HUI_WORK/.deps/ps5-opengl/libps5opengl-group.a")
stubs=("$opengl/lib/libSceAgc.so" "$opengl/lib/libSceAgcDriver.so")
stub_options=()
for stub in "${stubs[@]}"; do stub_options+=(--stub "$stub"); done
# The OpenGL runtime needs the kit's process heap (src/runtime/app_heap.c), and
# the splash stays until the first frame (src/runtime/runtime_shims.c).
wrap_options=(--wrap=sceSystemServiceHideSplashScreen)
for symbol in malloc calloc realloc free posix_memalign malloc_usable_size; do
    wrap_options+=("--wrap=$symbol")
done
ninja_inputs=("$native/ps5-pie.ld" "$native/app-symbols.map" "$sdk_root/bin/prospero-lld"
    "${link_inputs[@]}" "${stubs[@]}" "$sdk_root"/target/lib/*.so)
ninja_edge LINK "$build/llvm-pie.elf" "$sdk_root/bin/prospero-lld" \
    -T "$native/ps5-pie.ld" --eh-frame-hdr "${wrap_options[@]}" \
    --version-script "$native/app-symbols.map" \
    -e _start -o "$build/llvm-pie.elf" "${link_inputs[@]}" \
    --as-needed "${stubs[@]}" "$sdk_root"/target/lib/*.so
ninja_inputs=("$build/llvm-pie.elf" "$tool" "${stubs[@]}" "$sdk_root"/target/lib/*.so)
ninja_edge CONVERT "$build/eboot.elf" "$tool" link --in "$build/llvm-pie.elf" \
    --out "$build/eboot.elf" "${stub_options[@]}" --stub-dir "$sdk_root/target/lib" \
    --module-sdk "$module_sdk" --companion-sdk "$companion_sdk" --file-name eboot.elf
ninja_run

source "$app/tools/fonts.sh"

dist="$work/dist"
out="$dist/$title_id"
rm -rf -- "$out"
mkdir -p "$out/sce_sys" "$out/sce_module" "$out/assets/fonts" "$out/assets/audio/sfx"
"$tool" self --sign --in "$build/eboot.elf" --out "$out/eboot.bin" --magic "$fself_magic"
cp "$param" "$app/sce_sys/icon0.png" "$out/sce_sys/"
cp "$fonts"/*.huifont "$out/assets/fonts/"
cp "$kit"/third_party/fonts/{Inter,DejaVu}-LICENSE.txt "$out/assets/fonts/"
cp -R "$kit/assets/audio/sfx/glass" "$out/assets/audio/sfx/"
# Listing /app0 returns nothing on the console, so the sound folder carries an
# index of its files (read by save::list_files).
(cd "$out/assets/audio/sfx/glass" && find . -maxdepth 1 -type f ! -name index.txt -printf '%f\n' |
    LC_ALL=C sort > index.txt)
# Licences of everything in the app: Alembic, the kit, the OpenGL runtime.
mkdir -p "$out/licenses/ps5-homebrew-ui" "$out/licenses/ps5-opengl"
cp "$repo/LICENSE" "$repo/THIRD_PARTY_NOTICES.md" "$out/licenses/"
cp "$kit/LICENSE" "$kit/THIRD_PARTY_NOTICES.md" "$out/licenses/ps5-homebrew-ui/"
opengl_release=$(cd -P "$opengl/.." && pwd)
cp -R "$opengl_release/LICENSE" "$opengl_release/LICENSES" "$opengl_release/THIRD_PARTY_NOTICES.md" "$out/licenses/ps5-opengl/"
engine_elf=${ALEMBIC_ENGINE_ELF:-$repo/build-ps5/alembic.elf}
if [[ -f $engine_elf ]]; then
    cp "$engine_elf" "$out/assets/alembic.elf"
else
    echo "note: no engine payload ($engine_elf); the app will not start one itself" >&2
fi
cp "$HUI_WORK/runtime/libc.prx" "$out/sce_module/libc.prx"
"$tool" self --inspect --file "$out/eboot.bin" | grep -E 'integrity' >&2
(cd "$dist" && rm -f "$title_id.zip" && zip -qr -X "$title_id.zip" "$title_id")
echo "App folder: $out"
echo "ZIP: $dist/$title_id.zip"
