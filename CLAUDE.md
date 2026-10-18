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
```

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
  first, checked byte-for-byte against PS5PKGTool on a PC. A PS5 front end
  built on blackbearreloaded/ps5-homebrew-ui comes after that.
- **End goal: a PS5 homebrew.** The CLI is the test bed. Like
  PS5-LZ4-Compression, the engine will run as a payload ELF (which can reach
  `/data` and USB) with a ps5-homebrew-ui app as the front end.
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
- **Language rules:** C++20 with no exceptions and no RTTI (the PS5 build
  forbids them), so errors are returned as values.
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
