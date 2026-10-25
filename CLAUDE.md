# Alembic

A PS5 homebrew app that converts FPKG (debug PS5 packages) to and from other
formats: dump folders, exFAT, FFPKG (UFS2) and FFPFSC.

Read [docs/NOTES.md](docs/NOTES.md) first, starting with **"Where we are"** and
**"Next session"**: current capabilities, how everything is tested, how to
recreate the git-ignored reference checkouts, and the agreed next steps.

Build and test:

```sh
cmake -S . -B build && cmake --build build && ./build/alembic_tests
tools/oracle/compare.sh /path/to/PS5PKGTool   # byte-for-byte check vs PS5PKGTool
tools/pkg-oracle/compare.sh                   # FPKG reading vs ProsperoPkgTool/LibProsperoPKG
tools/pkg-oracle/check_samples.sh             # real files in .refs/samples/ (owner-supplied)
tools/lz4-oracle/compare.sh                   # LZ4 packs vs ampr_emu's ampr_pack.py (needs .refs/venv)
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
      -DLLVM_MINGW=$PWD/.refs/llvm-mingw-20261006-ucrt-macos-universal && cmake --build build-win   # Windows .exe
```

The console app is `app/` (a native PS5 title drawn with the vendored
`third_party/ps5-homebrew-ui`): `app/build.sh` builds it (on a Mac too, see
NOTES.md), `app/preview.sh [dir] [script]` draws it on this computer
against the README's stand-in engine. Look at the pictures after every UI
change. It mirrors the web page, both ways: a page change needs its app
counterpart and an app change its page counterpart, in the same commit.

The engine's web page is `src/engine/web` (Preact + TypeScript, bundled by
esbuild into one HTML file that CMake embeds; needs Node.js 20+ and npm).
After changing it: `cd src/engine/web && npx tsc --noEmit` and
`python3 tools/i18n_keys.py --check`. `node build.mjs --serve` there rebuilds
on every change and sends `/api` to a running engine. It must keep working in
the PS5's Safari-12-era browser: no regex lookbehind, no APIs newer than that.

Output must stay byte-identical to PS5PKGTool (the AMPR index with
`--ampr-scheme ps5pkgtool`), to ampr_emu's index builder (default scheme) and,
for `.ffpfs`, to MkPFS `pack folder --raw --no-compress`.
Run the oracle after changing any format code. Reference checkouts live in
`.refs/` (git-ignored): PS5PKGTool, ProsperoPkgTool, ampr_emu,
LibProsperoPKG-sven, fpkg-cli, PS5-LZ4-Compression, MkPFS (with its Python
environment in `.refs/mkpfs-venv`).

## Decisions so far

- **Name:** Alembic. "ProsperoPKGTool" was dropped because it clashes with
  pearlxcore/ProsperoPkgTool. The folder may still be called `ProsperoPKGTool`
  until it is renamed.
- **Architecture:** a portable C++20 core library and a PC command-line tool
  first, checked byte-for-byte against PS5PKGTool on a PC. On the console:
  the engine payload with its web page, and (from 0.3.0) a native app
  (`app/`, title PPSA99594) built on ps5-homebrew-ui that draws the same
  page and talks to the engine on 127.0.0.1:5940. Releases ship it as
  `alembic-app-<version>.ffpkg` (written by Alembic itself); engine and app
  share one version and one release.
- **End goal: a PS5 homebrew.** The CLI is the test bed. Like
  PS5-LZ4-Compression, the engine runs as a payload ELF (which can reach
  `/data` and USB); the app is only a front end and sends its bundled
  payload to the ELF loader (9021) when no engine answers.
- **Goal:** conversion between exFAT, FFPFSC and FPKG (folders are a side
  feature). folder/exFAT/FFPFSC conversions are done (`alembic convert`),
  byte-identical to PS5PKGTool. FPKG and FFPKG (UFS2) reading are done,
  checked on real files. FFPKG (UFS2) writing is byte-identical to PS5PKGTool
  and FFPFS (PFS) writing to MkPFS; FPKG writing (stored; `convert --to fpkg`, the engine and the page) is
  byte-identical to ProsperoPkgTool `img_create --seed S --no_right_sprx`.
- **Design:** every input is a `tree::Source` and every writer takes one, so
  conversions stream without temporary copies.
- **AMPR index:** a game's own `ampr_emu.index` is kept by default (byte-exact
  round trips); one is generated only when missing; `--ampr-index rebuild`
  matches PS5PKGTool/MkPFS, and the parity scripts pass it.
- **Compression:** PFSC uses all cores and zlib level 6 by default (owner
  decision: 2.4x faster than 9 for ~0.2% more size). Parity scripts pass
  `--level 9` to match PS5PKGTool.
- **Status:** an Alembic-made FFPFSC has been run on a PS5 via ShadowMountPlus.
- **LZ4 asset packs** (`ampr_assets.index` + `.pak` volumes, read by
  ampr_emu's PackedStdio runtime): `alembic convert ... --lz4`, ported from
  drakmor's `ampr_pack.py`, byte-identical to it (build ID included). The
  runtime `libSceAmpr.sprx` is never committed: the CLI takes it with
  `--lz4-runtime` (official ampr_emu release, SHA-256 pinned in NOTES.md).
- **AMPR emulator:** games whose eboot.bin imports libSceAmpr need
  ampr_emu's `libSceAmpr.sprx` in fakelib/ to start from an image or folder.
  Conversions add it (engine: on by default; CLI: `--ampr-emu FILE`) with an
  `ampr_emu.index`; `alembic ampr-emu` / the page's "Add the AMPR emulator"
  add it to an existing game (in place for a folder, as a ShadowMount+
  backport folder for an image). Never for FPKG output.
- **Engine protocol:** engines talk to each other through `/api/pair/...`
  and `/api/peer/...`, numbered by `peers::kPeerProtocol` (peers.hpp).
  Raise it whenever one of those requests or replies changes in a way the
  other engine must understand (a new required field, a changed meaning, a
  removed endpoint); engines with different numbers refuse each other and
  say which to update. Page fixes and new formats never change it.
- **Older apps:** an app older than its engine must still be able to notice
  and fix that. Every later engine keeps `/api/info`'s `version`, `started`
  and `update`, `GET /api/update` (its `app`, `app_pending`, `latest`,
  `current`), `POST /api/update/app`, `/api/update/install`,
  `/api/app/closing` and `/api/restart` working as they are (new fields are
  fine), and keeps telling the app by its User-Agent (`Alembic-app...`).
- **Updating itself:** the engine checks GitHub's latest release at start;
  Settings → Updates downloads the payload (checked against `SHA256SUMS` and
  the `ALEMBIC_PAYLOAD_VERSION:` marker in the ELF), replaces the payload
  files loaders start (Payload Manager slots renamed, their `meta.json`
  fixed by plain text replace; autoloader files keep their names), sends it
  to the ELF loader on 9021 and quits; a new engine stops an older one on
  its port first. The console app's image is replaced in the same update
  when its SHA-256 differs (staged under a '.' name, swapped once the app
  has been quiet for 15 s, then a ShadowMount+ rescan). See
  `src/engine/update.hpp`.
- **Language rules:** C++20 with no exceptions and no RTTI (the PS5 build
  forbids them), so errors are returned as values.
- **Windows:** cross-built with llvm-mingw, tested on a GitHub Windows runner
  (`tools/windows/smoke.sh`) and on the owner's PC. Files and folders go
  through `alembic/io.hpp` (UTF-8 paths with '/', `\\?\` long paths on
  Windows) and sockets through `src/engine/net.hpp`: no direct POSIX
  file, folder or socket calls (nor `fopen`) outside those and
  `platform_ps5.cpp`.
- **License:** GPL-3.0-or-later, compatible with every source we port from.
- **Third-party files at run time:** the engine's Components page downloads
  them on request from a fixed URL, pinned by SHA-256 (`src/engine/deps.cpp`):
  ampr_emu's `libSceAmpr.sprx` and ProsperoPkgTool's FPKG key moduli (needed
  for PKG output). Never commit them.

## Never

- Ship or depend on Oodle (`oo2core_*.dll`), `libScePubTools.dll`, or the
  closed binary LibProsperoPkg releases (the "Drakmor 0.6.x" line).
- Copy binaries or key material out of reference repos (for example
  LibProsperoPKG's `right.sprx`, `Keys/Data/*`, `pkg_meta_rsa_key.pem`)
  without first establishing where they come from.
- Launch anything on a console without asking the owner first.
- Put names of the owner's test files anywhere in the repo: no file names,
  game titles, title IDs or content IDs from `.refs/samples/` in code,
  docs, test fixtures, scripts or commit messages. Refer to them generically
  ("package A", "a 30 GB FFPFSC"). The same goes for anything that would
  reveal where they came from.

Porting from GPL-3.0 source is fine, including SvenGDK/LibProsperoPKG: keep
its NOTICE attributions (LibOrbisPkg, ooz).

**Repos with no license file** (rdmrocha/fpkg-cli,
juma-sayeh/PS5-Game-Compressor, and the `src/pfs_*` code of
PS5-LZ4-Compression that descends from it): no license means all rights
reserved, so **never copy or port their code**. Facts learned from their docs
(format details, console behaviour) are fine. Port from GPL/MIT sources only,
and list every source in THIRD_PARTY_NOTICES.md.

**No bundled binaries.** Third-party binaries the console needs (for example
ampr_emu's `libSceAmpr.sprx`) are fetched by a downloader in the app from the
official release, pinned by SHA-256, never committed to this repo.
