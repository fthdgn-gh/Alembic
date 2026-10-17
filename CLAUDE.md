# Alembic

A PS5 homebrew app that converts FPKG (debug PS5 packages) to and from other
formats: dump folders, exFAT, FFPKG (UFS2) and FFPFSC.

Read [docs/NOTES.md](docs/NOTES.md) first: it holds the research on the
reference repos, the decisions made so far, progress, and the next step.

Build and test:

```sh
cmake -S . -B build && cmake --build build && ./build/alembic_tests
tools/oracle/compare.sh /path/to/PS5PKGTool   # byte-for-byte check vs PS5PKGTool
```

Output must stay byte-identical to PS5PKGTool (the AMPR index with
`--ampr-scheme ps5pkgtool`) and to ampr_emu's index builder (default scheme).
Run the oracle after changing any format code. Reference checkouts live in
`.refs/` (git-ignored): PS5PKGTool, ProsperoPkgTool, ampr_emu,
LibProsperoPKG-sven, fpkg-cli, PS5-LZ4-Compression.

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
  byte-identical to PS5PKGTool. Next: FPKG reading, then FPKG building.
- **Design:** every input is a `tree::Source` and every writer takes one, so
  conversions stream without temporary copies.
- **Also wanted:** AMPR LZ4 asset packs (`ampr_assets.index` + `.pak`
  volumes), read by ampr_emu at runtime. See NOTES.md.
- **Language rules:** C++20 with no exceptions and no RTTI (the PS5 build
  forbids them), so errors are returned as values.
- **License:** GPL-3.0-or-later, compatible with every source we port from.

## Never

- Ship or depend on Oodle (`oo2core_*.dll`), `libScePubTools.dll`, or the
  closed binary LibProsperoPkg releases (the "Drakmor 0.6.x" line).
- Copy binaries or key material out of reference repos (for example
  LibProsperoPKG's `right.sprx`, `Keys/Data/*`, `pkg_meta_rsa_key.pem`)
  without first establishing where they come from.
- Launch anything on a console without asking the owner first.

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
