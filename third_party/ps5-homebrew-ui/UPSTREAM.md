# ps5-homebrew-ui in Alembic

Vendored from blackbearreloaded/ps5-homebrew-ui at `bbe1905`
("Draw and measure a paragraph from the same lines (#4)", 2026-10-13), the
last upstream commit before the repository and its account left GitHub.
Mirrors with the full history end at the same commit. GPL-3.0-or-later
(`LICENSE`); third-party parts in `THIRD_PARTY_NOTICES.md`.

Kept: `src/{gfx,ui,core,audio,platform,runtime,third_party}` (the kit and the
PS5 platform layer), `tooling/` and the build scripts in `tools/` (from
ps5-native-app-boilerplate), `runtime/libc.prx.sha256`, `host/platform_host.cpp`,
the baked fonts and their sources and licences, the "glass" sound set, and
the font baker and `tools/zip-open-modes.py`. Left out: the demo app (`src/app`, `src/concepts`,
`src/demo`, `src/main.cpp`), its tests, docs, music, home-screen art and the
other sound set.

Alembic's app (`app/`) builds it with `app/build.sh`, and previews it on a PC
with `app/preview.sh`.

## Local changes

All in `tools/`, each marked with an "Alembic:" comment:

- `build-host-tools.sh`, `rebuild-libc.sh`, `fetch-opengl-sdk.sh`,
  `prepare-opengl.sh`, `setup-native-dependencies.sh`: downloads and build
  output go to `$HUI_WORK` (default: this folder, as before), so nothing is
  written into the repository; `rebuild-libc.sh` installs and checks
  `libc.prx` there.
- `fetch-opengl-sdk.sh`: the ps5-opengl 1.0.2 archive comes from two copies
  of the original release (JMUtechnologies/ps5-opengl, darkxex/ps5-opengl),
  accepted only with the SHA-256 pinned here; the original URL is gone.
- `setup-native-dependencies.sh`: zlib is built with `ARFLAGS=rc` (on macOS
  zlib's configure passes libtool flags to llvm-ar).
- `prepare-opengl.sh`: `PS5_CLANG_BUILTINS` names clang's x86-64 builtins
  archive where clang has none (Homebrew's LLVM on a Mac).
