#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Cross-builds Alembic for the PS5 with ps5-payload-dev/sdk into build-ps5/.
#   tools/ps5/build.sh [extra cmake --build args]
# PS5_PAYLOAD_SDK defaults to .refs/ps5-payload-sdk (the v0.44 release zip,
# unpacked). LLVM 18 comes from LLVM_CONFIG, else Homebrew's llvm@18 or
# llvm-config-18 / llvm-config on the PATH.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
export PS5_PAYLOAD_SDK="${PS5_PAYLOAD_SDK:-$repo/.refs/ps5-payload-sdk}"
if [[ -z ${LLVM_CONFIG:-} ]]; then
    for c in /opt/homebrew/opt/llvm@18/bin/llvm-config llvm-config-18 llvm-config; do
        if command -v "$c" >/dev/null 2>&1; then LLVM_CONFIG="$(command -v "$c")"; break; fi
    done
fi
: "${LLVM_CONFIG:?LLVM 18 not found; set LLVM_CONFIG}"
# The SDK's wrappers run it from other folders, so it must be a full path.
[[ $LLVM_CONFIG == /* ]] || LLVM_CONFIG="$(command -v "$LLVM_CONFIG")" || { echo "$LLVM_CONFIG not found" >&2; exit 1; }
export LLVM_CONFIG
[[ -x $PS5_PAYLOAD_SDK/bin/prospero-cmake ]] || { echo "no PS5 payload SDK at $PS5_PAYLOAD_SDK" >&2; exit 1; }
"$PS5_PAYLOAD_SDK/bin/prospero-cmake" -S "$repo" -B "$repo/build-ps5" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_VERBOSE_MAKEFILE=OFF >/dev/null
cmake --build "$repo/build-ps5" -j "$@"
