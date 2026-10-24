# SPDX-License-Identifier: GPL-3.0-or-later
# Sourced by app/build.sh and app/preview.sh: the tools the kit's scripts
# expect, on Linux and on a Mac. Needs $work (build-app/).

# clang 18 for the console (the kit's validated compiler).
if [[ -z ${PS5_CLANG:-} ]]; then
    for c in clang-18 /opt/homebrew/opt/llvm@18/bin/clang-18 /usr/local/opt/llvm@18/bin/clang-18; do
        if command -v "$c" >/dev/null 2>&1; then PS5_CLANG=$(command -v "$c"); break; fi
    done
fi
: "${PS5_CLANG:?clang 18 not found (apt install clang-18 lld-18, or brew install llvm@18); or set PS5_CLANG}"
export PS5_CLANG
# The kit's scripts look for clang-18 on the PATH.
PATH="$(dirname "$PS5_CLANG"):$PATH"

if [[ $(uname -s) == Darwin ]]; then
    # GNU sha256sum --strict, stat -c, find -printf; Homebrew's bash 4+.
    for p in coreutils findutils; do
        d="$(brew --prefix "$p" 2>/dev/null)/libexec/gnubin"
        [[ -d $d ]] || { echo "brew install $p" >&2; exit 2; }
        PATH="$d:$PATH"
    done
    PATH="$(brew --prefix)/bin:$PATH"
    : "${HOST_CXX:=/usr/bin/clang++}"
    : "${HOST_CC:=/usr/bin/clang}"
    # Homebrew's clang has no x86-64 Linux builtins; take the archive from
    # Ubuntu 24.04's libclang-rt-18-dev (what the Linux build links), pinned.
    if [[ -z ${PS5_CLANG_BUILTINS:-} ]]; then
        rt="$work/deps/clang-rt"
        PS5_CLANG_BUILTINS="$rt/libclang_rt.builtins-x86_64.a"
        if [[ ! -f $PS5_CLANG_BUILTINS ]]; then
            deb=libclang-rt-18-dev_18.1.3-1_amd64.deb
            sha=2e406b65eb8426b8988ca28bcf6db8568348c8e3fb3bda8c034104a2b4f32ba4
            rm -rf "$rt" && mkdir -p "$rt/x"
            curl -fsSL -o "$rt/$deb" "http://archive.ubuntu.com/ubuntu/pool/universe/l/llvm-toolchain-18/$deb"
            echo "$sha  $rt/$deb" | sha256sum --check --strict --quiet
            (cd "$rt/x" && ar x "../$deb" && tar -xf data.tar.* \
                ./usr/lib/llvm-18/lib/clang/18/lib/linux/libclang_rt.builtins-x86_64.a)
            mv "$rt/x/usr/lib/llvm-18/lib/clang/18/lib/linux/libclang_rt.builtins-x86_64.a" "$PS5_CLANG_BUILTINS"
            rm -rf "$rt/x" "$rt/$deb"
        fi
    fi
    export PS5_CLANG_BUILTINS
fi
if [[ -z ${HOST_CXX:-} ]]; then
    for c in clang++ clang++-18 g++; do command -v "$c" >/dev/null 2>&1 && { HOST_CXX=$c; break; }; done
fi
if [[ -z ${HOST_CC:-} ]]; then
    for c in clang clang-18 gcc; do command -v "$c" >/dev/null 2>&1 && { HOST_CC=$c; break; }; done
fi
export USE_CCACHE="${USE_CCACHE:-0}"
export PATH HOST_CXX HOST_CC CXX="${CXX:-$HOST_CXX}"
for c in ninja python3 curl unzip zip wget; do
    command -v "$c" >/dev/null || { echo "missing: $c" >&2; exit 2; }
done
