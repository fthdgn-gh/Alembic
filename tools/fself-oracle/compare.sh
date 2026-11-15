#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Fake-signed SELFs: Alembic against ampr-emu-ng's tools/make_fself.py
# --ptype fake (what ps5-dump-forge's fself.rs matches), on synthetic PS5
# ELFs (make_elfs.py). Every SELF must be byte for byte the same, through
# `alembic fself` and through a conversion (folder -> folder and -> exFAT).
#   tools/fself-oracle/compare.sh [path/to/ampr-emu-ng]
# Needs a configured build in ./build and Python 3.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
ng="${1:-$repo/.refs/ampr-emu-ng}"
script="$ng/tools/make_fself.py"
[[ -f $script ]] || { echo "no make_fself.py at $script" >&2; exit 1; }
alembic="$repo/build/alembic"
[[ -x $alembic ]] || { echo "build Alembic first (cmake --build build)" >&2; exit 1; }
work="$(mktemp -d "${TMPDIR:-/tmp}/alembic-fself-oracle-XXXXXX")"
trap '[[ -n ${KEEP:-} ]] || rm -rf "$work"' EXIT
python3 -I "$here/make_elfs.py" "$work/in"
fail=0
mkdir -p "$work/ref" "$work/ours" "$work/game/sce_sys" "$work/game/sce_module"
for elf in "$work"/in/*; do
    name="$(basename "$elf")"
    python3 -I "$script" --ptype fake "$elf" "$work/ref/$name" >/dev/null
    "$alembic" fself "$elf" "$work/ours/$name" >/dev/null
    if cmp -s "$work/ref/$name" "$work/ours/$name"; then echo "same  $name"; else echo "DIFF  $name"; fail=1; fi
done

# A game with them: eboot.bin and sce_module/<name>.prx, served fake-signed.
cp "$work/in/eboot.elf" "$work/game/eboot.bin"
for elf in "$work"/in/*; do
    name="$(basename "$elf")"
    [[ $name == eboot.elf ]] || cp "$elf" "$work/game/sce_module/${name%.*}.prx"
done
printf '{"titleId":"ABCD99999","contentVersion":"01.000.000"}' >"$work/game/sce_sys/param.json"
"$alembic" convert "$work/game" "$work/out/" >/dev/null
"$alembic" convert "$work/game" "$work/game.exfat" >/dev/null
"$alembic" convert "$work/game.exfat" "$work/from-exfat/" >/dev/null
for out in out from-exfat; do
    check() { if cmp -s "$1" "$2"; then echo "same  $out/$3"; else echo "DIFF  $out/$3"; fail=1; fi; }
    check "$work/ref/eboot.elf" "$work/$out/eboot.bin" eboot.bin
    for elf in "$work"/in/*; do
        name="$(basename "$elf")"
        [[ $name == eboot.elf ]] || check "$work/ref/$name" "$work/$out/sce_module/${name%.*}.prx" "sce_module/${name%.*}.prx"
    done
done
# The dumper's style (decrypt_self): `alembic fself --elf2fself` against
# EchoStretch/ps5-app-dumper's source/elf2fself.c, built here with stand-ins
# for its console logging and the ELF types macOS lacks.
dumper="${ALEMBIC_APP_DUMPER:-$repo/.refs/ps5-app-dumper}"
if [[ -f $dumper/source/elf2fself.c ]]; then
    mkdir -p "$work/stub" "$work/dump-ref" "$work/dump-ours"
    cat >"$work/stub/elf.h" <<'C'
#include <stdint.h>
#define PT_LOAD 1
typedef struct { unsigned char e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version;
  uint64_t e_entry, e_phoff, e_shoff; uint32_t e_flags;
  uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t p_type, p_flags; uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align; } Elf64_Phdr;
C
    cat >"$work/stub/utils.h" <<'C'
void write_log(const char *path, const char *fmt, ...);
void printf_notification(const char *fmt, ...);
C
    cat >"$work/stub/main.c" <<'C'
#include <stdio.h>
char g_log_path[512];
int g_enable_logging;
void write_log(const char *path, const char *fmt, ...) { (void)path; (void)fmt; }
void printf_notification(const char *fmt, ...) { (void)fmt; }
int elf2fself(const char *elf_path, const char *fself_path);
int main(int argc, char **argv) { return argc == 3 && elf2fself(argv[1], argv[2]) == 0 ? 0 : 1; }
C
    # -ftrivial-auto-var-init=zero: elf2fself reads its version header unset
    # when an ELF has none.
    cc -w -O1 -ftrivial-auto-var-init=zero -I"$work/stub" -I"$dumper/include" -o "$work/elf2fself" \
        "$work/stub/main.c" "$dumper/source/elf2fself.c" "$dumper/source/sha256.c"
    # elf2fself puts the ex-info after the program headers as written right
    # after the ELF header, so an ELF whose e_phoff is not 0x40 or whose
    # e_ehsize is larger comes out misplaced there (game ELFs have both 0x40):
    # not compared. And it ends the file at the last data byte, where the
    # header's file_size (where the PT_SCE_VERSION bytes are read from) is
    # 16-aligned: ours puts them there (same_as_elf2fself.py checks that).
    for elf in "$work"/in/*; do
        name="$(basename "$elf")"
        case $name in late_phdr.elf|big_ehsize.elf) echo "n/a   elf2fself $name"; continue ;; esac
        "$work/elf2fself" "$elf" "$work/dump-ref/$name"
        "$alembic" fself --elf2fself "$elf" "$work/dump-ours/$name" >/dev/null
        if python3 -I "$here/same_as_elf2fself.py" "$work/dump-ref/$name" "$work/dump-ours/$name"; then echo "same  elf2fself $name"; else echo "DIFF  elf2fself $name"; fail=1; fi
    done
else
    echo "skip  elf2fself (no ps5-app-dumper at $dumper)"
fi
[[ $fail == 0 ]] && echo "all fself checks passed" || { echo "fself checks FAILED"; exit 1; }
